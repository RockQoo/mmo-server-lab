#include "Base/RuidGenerator.h"

#include <chrono>
#include <optional>
#include <thread>
#include <utility>

#include "Base/Assert.h"

namespace Base
{
    namespace
    {
        constexpr int64_t SequenceMask{Ruid::MaxSequence};

        // 프로세스 전역 발급기 자리. InitializeGlobal이 한 번 채우고, Global이 꺼낸다.
        struct GlobalSlot
        {
            GlobalSlot() = default;
            GlobalSlot(const GlobalSlot&) = delete;
            GlobalSlot& operator=(const GlobalSlot&) = delete;
            GlobalSlot(GlobalSlot&&) = delete;
            GlobalSlot& operator=(GlobalSlot&&) = delete;

            std::optional<RuidGenerator> generator{};
            std::atomic<RuidGenerator*> published{};
            std::atomic<bool> claimed{};
        };

        GlobalSlot& Slot()
        {
            static GlobalSlot s_slot; // 함수 안 static: 처음 부를 때 한 번만 만들어진다(스레드 안전)
            return s_slot;
        }
    } // namespace

    RuidGenerator::RuidGenerator(const int32_t realm, const int32_t node) : RuidGenerator(realm, node, SteadyClock())
    {
    }

    RuidGenerator::RuidGenerator(const int32_t realm, const int32_t node, Clock clock)
        : _clock{std::move(clock)}, _realm{realm}, _node{node}
    {
        ASSERT(realm >= Ruid::MinRealm && realm <= Ruid::MaxRealm, "RuidGenerator realm out of range");
        ASSERT(node >= 0 && node <= Ruid::MaxNode, "RuidGenerator node out of range");
        ASSERT(_clock != nullptr, "RuidGenerator clock is empty");
    }

    Ruid RuidGenerator::Next()
    {
        // 순서 보장이 필요한 다른 메모리가 없고 _last 하나만 다투므로 relaxed로 충분하다.
        // (한 원자 변수에 대한 CAS들은 모든 스레드가 같은 순서로 본다)
        auto last = _last.load(std::memory_order_relaxed);
        auto waited = false; // 이번 호출에서 이미 기다림을 셌는가(카운터는 호출당 한 번만 쓴다. Codex 리뷰 R1)
        for (;;)
        {
            const auto now = _clock();
            ASSERT(now >= 0 && now <= Ruid::MaxTimestamp, "Ruid clock out of range");

            const auto lastTime = last >> Ruid::SequenceBits; // -1이면 산술 시프트로 -1
            const auto lastSequence = last & SequenceMask;

            auto time = now;
            auto sequence = int64_t{0};
            if (now <= lastTime)
            {
                // 같은 ms. (now < lastTime은 기본 시계에선 없고, 가짜 시계가 뒤로 갈 때만 생긴다.
                // 그때도 마지막 시각을 유지해 겹치지 않고 커지게 한다)
                time = lastTime;
                if (lastSequence == SequenceMask)
                {
                    // 이 ms의 512개를 다 썼다. 다음 ms가 될 때까지 양보한다.
                    if (!waited)
                    {
                        waited = true;
                        _sequenceWaits.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::yield();
                    last = _last.load(std::memory_order_relaxed);
                    continue;
                }
                sequence = lastSequence + 1;
            }

            const auto next = (time << Ruid::SequenceBits) | sequence;
            // 실패하면 last에 지금 값이 들어온다. 그 값으로 다시 계산한다.
            if (_last.compare_exchange_weak(last, next, std::memory_order_relaxed))
            {
                return Ruid::Make(time, _realm, _node, static_cast<int32_t>(sequence));
            }
        }
    }

    int32_t RuidGenerator::Realm() const noexcept
    {
        return _realm;
    }

    int32_t RuidGenerator::Node() const noexcept
    {
        return _node;
    }

    int64_t RuidGenerator::SequenceWaitCount() const noexcept
    {
        return _sequenceWaits.load(std::memory_order_relaxed);
    }

    RuidGenerator::Clock RuidGenerator::SteadyClock()
    {
        using namespace std::chrono;
        const auto startUnixMs = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        const auto startElapsedMs = startUnixMs - Ruid::EpochUnixMs;
        const auto startSteady = steady_clock::now();
        return [startElapsedMs, startSteady]
        { return startElapsedMs + duration_cast<milliseconds>(steady_clock::now() - startSteady).count(); };
    }

    void RuidGenerator::InitializeGlobal(const int32_t realm, const int32_t node)
    {
        auto& slot = Slot();
        const auto alreadyClaimed = slot.claimed.exchange(true);
        // 두 번째 호출을 허용하면 다른 번호로 바뀌거나, 순번이 처음부터 다시 나가 이미 낸 ID와 겹친다.
        ASSERT(!alreadyClaimed, "Ruid::Init (RuidGenerator::InitializeGlobal) called twice");
        slot.generator.emplace(realm, node);
        slot.published.store(&*slot.generator, std::memory_order_release);
    }

    RuidGenerator& RuidGenerator::Global()
    {
        auto* const generator = Slot().published.load(std::memory_order_acquire);
        ASSERT(generator != nullptr, "Ruid::Init (RuidGenerator::InitializeGlobal) not called");
        return *generator;
    }

    void Ruid::Init(const int32_t realm, const int32_t node)
    {
        RuidGenerator::InitializeGlobal(realm, node);
    }

    Ruid Ruid::Create()
    {
        return RuidGenerator::Global().Next();
    }
} // namespace Base
