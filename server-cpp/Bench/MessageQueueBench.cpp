#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// GetProcessTimes(프로세스 전체 CPU 시간). Lane.cpp 와 같이 windows.h 의 min/max 매크로와 잘 안 쓰는 부분은 끈다.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Base/ByteReader.h"
#include "Base/ByteWriter.h"
#include "Memory/BufferPool.h"
#include "Memory/ObjectPool.h"
#include "Messaging/Lane.h"
#include "Messaging/LaneGroup.h"
#include "ThreadTeam.h"

// 메시지 큐에 객체를 넣을까, 직렬화한 바이트를 넣을까. 결과는 docs/decisions/002-message-queue-object-vs-bytes.md.
//
// 모양: 생산자 스레드 P개(ThreadTeam)가 Lane 하나에 메시지를 넣고, Lane 스레드가 꺼내 처리한다. 측정 한 번 =
// 생산자가 모두 넣고 + Lane이 모두 처리할 때까지(마지막에 넣은 표시 일이 끝날 때까지).
// 생산자는 자기가 넣고 아직 처리되지 않은 메시지가 Window개를 넘으면 기다린다(서버의 흐름 제어. 없으면 큐와
// 메시지가 끝없이 쌓여 메모리 측정이 된다).
//
// 세 방식(+ 기준 하나). 생산자는 원본 메시지(source)를 갖고 있고 그 내용을 Lane에 보낸다.
//   QueueOnly   빈 일만 넣는다. 큐 자체(mutex + move_only_function)의 비용
//   New         make_unique로 복사해 unique_ptr을 move. 처리 뒤 delete
//   Pool        ObjectPool에서 빌려 복사(내부 string·vector 용량 재사용)해 move. 처리 뒤 Lane 스레드에서 반납
//   Bytes       ByteWriter로 쓰고 BufferPool 버퍼에 복사해 move. Lane이 ByteReader로 새 객체에 읽어 처리
//               (ByteWriter는 자기 vector에 쓰므로 PooledBuffer로 한 번 더 복사한다. 생산자마다 writer 하나를 다시
//               쓴다)
// 처리(Consume)는 모든 필드를 읽어 합친다. 네 방식 모두 같은 함수다.
//
// 메시지 세 가지(직렬화 크기)
//   Move       이동 명령: id + 좌표 3개 + 플래그, 24B. 내부 할당 없음
//   Chat       채팅: id + 문자열 96자, 106B. 문자열 할당 1번
//   Inventory  인벤토리 일부: 주인 + 칸 60개(칸마다 16B), 970B. vector 할당 1번

namespace
{
    constexpr int64_t TotalMessages = int64_t{1} << 20;
    constexpr int64_t Window = 1024;   // 생산자 하나가 처리 안 된 채로 둘 수 있는 메시지 수
    constexpr int64_t CheckEvery = 64; // 흐름 제어 확인 간격(매번 읽으면 Lane이 쓰는 줄을 계속 읽는다)

    struct Move
    {
        uint64_t id{};
        float x{};
        float y{};
        float z{};
        uint32_t flags{};

        SERIALIZE_FIELDS(id, x, y, z, flags)

        void Reset() noexcept
        {
            *this = Move{};
        }
        static Move Sample()
        {
            return {.id = 1, .x = 1.0F, .y = 2.0F, .z = 3.0F, .flags = 7};
        }
        [[nodiscard]] uint64_t Sum() const noexcept
        {
            return id + static_cast<uint64_t>(x + y + z) + flags;
        }
    };

    struct Chat
    {
        uint64_t id{};
        std::string text;

        SERIALIZE_FIELDS(id, text)

        void Reset() noexcept
        {
            id = 0;
            text.clear();
        }
        static Chat Sample()
        {
            return {.id = 1, .text = std::string(96, 'a')};
        }
        [[nodiscard]] uint64_t Sum() const noexcept
        {
            auto sum = id;
            for (const auto c : text)
            {
                sum += static_cast<uint8_t>(c);
            }
            return sum;
        }
    };

    struct Slot
    {
        uint32_t itemId{};
        uint32_t count{};
        uint64_t uid{};

        SERIALIZE_FIELDS(itemId, count, uid)
    };

    struct Inventory
    {
        uint64_t owner{};
        std::vector<Slot> slots;

        SERIALIZE_FIELDS(owner, slots)

        void Reset() noexcept
        {
            owner = 0;
            slots.clear();
        }
        static Inventory Sample()
        {
            auto inventory = Inventory{.owner = 1, .slots = {}};
            for (auto i = uint32_t{0}; i < 60; ++i)
            {
                inventory.slots.push_back({.itemId = i, .count = i + 1, .uid = uint64_t{i} * 1000});
            }
            return inventory;
        }
        [[nodiscard]] uint64_t Sum() const noexcept
        {
            auto sum = owner;
            for (const auto& slot : slots)
            {
                sum += slot.itemId + slot.count + slot.uid;
            }
            return sum;
        }
    };

    // 생산자 하나의 흐름 제어 + Lane이 쓰는 합계. 두 값 모두 Lane 스레드만 쓴다.
    // 128B 간격: 이웃 생산자의 Flow와 같은 캐시 줄에 놓이지 않게(alignas는 C4324 경고라 패딩으로 띄운다).
    struct Flow
    {
        Flow() = default;
        ~Flow() = default;
        Flow(const Flow&) = delete;
        Flow& operator=(const Flow&) = delete;
        Flow(Flow&&) = delete;
        Flow& operator=(Flow&&) = delete;

        std::atomic<int64_t> processed{};
        uint64_t checksum{};
        std::array<std::byte, 112> padding{};

        template <typename Msg>
        void Consume(const Msg& message) noexcept
        {
            checksum += message.Sum();
            processed.store(processed.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        }
    };

    struct CpuTime
    {
        double userNs{};
        double kernelNs{};
    };

    // 이 프로세스의 모든 스레드가 지금까지 쓴 CPU 시간(GetProcessTimes, 100ns 단위)
    CpuTime ProcessCpu()
    {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
        {
            return {};
        }
        const auto toNs = [](const FILETIME& time)
        {
            const auto ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
            return static_cast<double>(ticks) * 100.0;
        };
        return {.userNs = toNs(user), .kernelNs = toNs(kernel)};
    }

    void PostOrDie(Messaging::Lane& lane, Messaging::Lane::Job job)
    {
        if (!lane.Post(std::move(job)))
        {
            std::terminate(); // 측정 중 Lane은 멈추지 않는다
        }
    }

    // ---- 보내는 방식. 생산자마다 하나 만들고 Make를 반복해 부른다. Make가 보낼 준비(생산자 쪽 일)를 하고, 돌려준 일이
    // 받는 쪽 일(Lane에서 실행)이다.

    struct QueueOnly
    {
        static Messaging::Lane::Job Make(Flow& flow)
        {
            return [&flow]
            {
                ++flow.checksum;
                flow.processed.store(flow.processed.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            };
        }
    };

    template <typename Msg>
    struct New
    {
        Msg source{Msg::Sample()};

        Messaging::Lane::Job Make(Flow& flow)
        {
            auto message = std::make_unique<Msg>(source);
            return [&flow, message = std::move(message)] { flow.Consume(*message); };
        }
    };

    template <typename Msg>
    struct Pool
    {
        Msg source{Msg::Sample()};

        Messaging::Lane::Job Make(Flow& flow)
        {
            auto message = Memory::ObjectPool<Msg>::Shared().Rent();
            *message = source; // 복사 대입: 빌린 객체의 string·vector 용량을 다시 쓴다
            return [&flow, message = std::move(message)] { flow.Consume(*message); };
        }
    };

    template <typename Msg>
    struct Bytes
    {
        Msg source{Msg::Sample()};
        Base::ByteWriter writer;

        Messaging::Lane::Job Make(Flow& flow)
        {
            writer.Clear();
            writer << source;
            const auto data = writer.Data();
            auto buffer = Memory::BufferPool::Shared().Rent(data.size());
            std::memcpy(buffer.Data(), data.data(), data.size());
            return [&flow, buffer = std::move(buffer), length = data.size()]
            {
                auto message = Msg{};
                auto reader = Base::ByteReader{std::span<const std::byte>{buffer.Data(), length}};
                reader >> message;
                if (!reader.IsValid())
                {
                    std::terminate();
                }
                flow.Consume(message);
            };
        }
    };

    // ---- 측정

    // 생산자 N개 → Lane M개(LaneGroup). 생산자는 메시지마다 키(플레이어 ID처럼 계속 바뀌는 값)로 Lane을 고른다.
    // 흐름 제어 카운터는 (생산자, Lane) 쌍마다 따로 둔다: 카운터를 쓰는 스레드가 늘 Lane 하나라 원자적 더하기가 필요
    // 없다. 생산자별 카운터 하나를 Lane 여러 개가 같이 쓰면 측정 틀이 경합을 만들어 결과에 섞인다.
    template <typename Sender>
    void MessageQueue(benchmark::State& state)
    {
        const auto producers = static_cast<std::size_t>(state.range(0));
        const auto lanes = static_cast<std::size_t>(state.range(1));
        const auto perProducer = TotalMessages / static_cast<int64_t>(producers);
        Messaging::LaneGroup group("bench-lane", lanes);
        Bench::ThreadTeam team(static_cast<int32_t>(producers));
        const auto flows = std::make_unique<Flow[]>(producers * lanes); // [생산자 * lanes + Lane]
        std::atomic<int64_t> waits{}; // 생산자가 흐름 제어로 기다린 횟수(Lane이 못 따라온 정도)
        const auto cpuBefore = ProcessCpu();

        for (auto iteration : state)
        {
            team.Run(
                [&group, &flows, &waits, lanes, perProducer](const int32_t producer)
                {
                    auto myWaits = int64_t{0};
                    auto* const myFlows = &flows[static_cast<std::size_t>(producer) * lanes];
                    auto sent = std::vector<int64_t>(lanes); // Lane마다 보낸 수
                    for (auto l = std::size_t{0}; l < lanes; ++l)
                    {
                        myFlows[l].processed.store(0, std::memory_order_relaxed); // 이전 측정은 모두 처리됐다(latch)
                    }
                    auto sender = Sender{};
                    // 생산자마다 다른 키 범위. LaneGroup이 섞어서 나누므로 Lane이 골고루 섞인다.
                    const auto keyBase = static_cast<uint64_t>(producer) << 32U;
                    for (auto i = int64_t{0}; i < perProducer; ++i)
                    {
                        const auto key = keyBase + static_cast<uint64_t>(i);
                        const auto l = Messaging::LaneGroup::IndexFor(key, lanes);
                        auto& flow = myFlows[l];
                        if (sent[l] % CheckEvery == 0)
                        {
                            while (sent[l] - flow.processed.load(std::memory_order_acquire) >= Window)
                            {
                                ++myWaits;
                                std::this_thread::yield();
                            }
                        }
                        ++sent[l];
                        PostOrDie(group.At(l), sender.Make(flow));
                    }
                    waits.fetch_add(myWaits, std::memory_order_relaxed); // 측정 한 번에 한 번
                });
            // Lane은 넣은 순서대로 처리하므로, Lane마다 마지막에 넣은 이 일이 끝나면 앞의 메시지도 모두 처리됐다.
            std::latch drained{static_cast<std::ptrdiff_t>(lanes)};
            for (auto l = std::size_t{0}; l < lanes; ++l)
            {
                PostOrDie(group.At(l), [&drained] { drained.count_down(); });
            }
            drained.wait();
        }

        auto checksum = uint64_t{0};
        for (auto f = std::size_t{0}; f < producers * lanes; ++f)
        {
            checksum += flows[f].checksum;
        }
        benchmark::DoNotOptimize(checksum); // 측정 반복 밖에서 한 번
        const auto messages = state.iterations() * perProducer * static_cast<int64_t>(producers);
        state.SetItemsProcessed(messages);

        // 프로세스 전체 CPU 시간(모든 스레드 합). 벽시계 시간은 가장 느린 쪽에 묶이지만, 이 값은 메시지 하나에 모든
        // 스레드가 쓴 CPU다. kernel은 깨우기(notify)·기다리기·yield처럼 OS를 부른 시간이다.
        const auto cpuAfter = ProcessCpu();
        const auto perMessage = [messages](const double ns) { return ns / static_cast<double>(messages); };
        state.counters["user_ns"] = perMessage(cpuAfter.userNs - cpuBefore.userNs);
        state.counters["kernel_ns"] = perMessage(cpuAfter.kernelNs - cpuBefore.kernelNs);
        state.counters["waits"] = static_cast<double>(waits.load()) / static_cast<double>(state.iterations());
    }

    // ---- 나눠 보기(스레드 하나, 큐 없음)

    // 큐 없이 보내는 쪽 일 + 받는 쪽 일을 같은 스레드에서 바로 잇는다. 메시지 하나의 CPU 비용(큐·깨우기 제외).
    // 같은 스레드에서 반납하므로 풀은 늘 스레드 캐시에서 끝난다(큐를 거칠 때보다 유리하다).
    template <typename Sender>
    void NoQueue(benchmark::State& state)
    {
        const auto flow = std::make_unique<Flow>();
        auto sender = Sender{};
        for (auto iteration : state)
        {
            auto job = sender.Make(*flow);
            job();
        }
        benchmark::DoNotOptimize(flow->checksum);
        state.SetItemsProcessed(state.iterations());
    }

    // 직렬화 쓰기만: writer 재사용 + 쓰기
    template <typename Msg>
    void WriteOnly(benchmark::State& state)
    {
        const auto source = Msg::Sample();
        auto writer = Base::ByteWriter{};
        for (auto iteration : state)
        {
            writer.Clear();
            writer << source;
            benchmark::DoNotOptimize(writer.Data().data());
        }
        state.SetItemsProcessed(state.iterations());
    }

    // 직렬화 읽기만: 새 객체에 읽기
    template <typename Msg>
    void ReadOnly(benchmark::State& state)
    {
        auto writer = Base::ByteWriter{};
        writer << Msg::Sample();
        const auto data = writer.Data();
        for (auto iteration : state)
        {
            auto message = Msg{};
            auto reader = Base::ByteReader{data};
            reader >> message;
            benchmark::DoNotOptimize(message);
        }
        state.SetItemsProcessed(state.iterations());
    }
} // namespace

// 인자: 생산자 스레드 수 N, Lane 수 M(Lane마다 스레드 하나). 물리 코어 8개라 8:4(12스레드)는 코어가 모자란 경우다
#define MESSAGE_QUEUE_BENCH(sender, name)                                                                              \
    BENCHMARK(MessageQueue<sender>)                                                                                    \
        ->Name(name)                                                                                                   \
        ->ArgNames({"producers", "lanes"})                                                                             \
        ->Args({1, 1})                                                                                                 \
        ->Args({4, 1})                                                                                                 \
        ->Args({2, 2})                                                                                                 \
        ->Args({4, 2})                                                                                                 \
        ->Args({4, 4})                                                                                                 \
        ->Args({8, 4})                                                                                                 \
        ->UseRealTime()                                                                                                \
        ->Unit(benchmark::kMillisecond)

MESSAGE_QUEUE_BENCH(QueueOnly, "MessageQueue/QueueOnly");
MESSAGE_QUEUE_BENCH(New<Move>, "MessageQueue/Move/New");
MESSAGE_QUEUE_BENCH(Pool<Move>, "MessageQueue/Move/Pool");
MESSAGE_QUEUE_BENCH(Bytes<Move>, "MessageQueue/Move/Bytes");
MESSAGE_QUEUE_BENCH(New<Chat>, "MessageQueue/Chat/New");
MESSAGE_QUEUE_BENCH(Pool<Chat>, "MessageQueue/Chat/Pool");
MESSAGE_QUEUE_BENCH(Bytes<Chat>, "MessageQueue/Chat/Bytes");
MESSAGE_QUEUE_BENCH(New<Inventory>, "MessageQueue/Inventory/New");
MESSAGE_QUEUE_BENCH(Pool<Inventory>, "MessageQueue/Inventory/Pool");
MESSAGE_QUEUE_BENCH(Bytes<Inventory>, "MessageQueue/Inventory/Bytes");

#define NO_QUEUE_BENCH(sender, name) BENCHMARK(NoQueue<sender>)->Name(name)
NO_QUEUE_BENCH(QueueOnly, "MessageNoQueue/Empty");
NO_QUEUE_BENCH(New<Move>, "MessageNoQueue/Move/New");
NO_QUEUE_BENCH(Pool<Move>, "MessageNoQueue/Move/Pool");
NO_QUEUE_BENCH(Bytes<Move>, "MessageNoQueue/Move/Bytes");
NO_QUEUE_BENCH(New<Chat>, "MessageNoQueue/Chat/New");
NO_QUEUE_BENCH(Pool<Chat>, "MessageNoQueue/Chat/Pool");
NO_QUEUE_BENCH(Bytes<Chat>, "MessageNoQueue/Chat/Bytes");
NO_QUEUE_BENCH(New<Inventory>, "MessageNoQueue/Inventory/New");
NO_QUEUE_BENCH(Pool<Inventory>, "MessageNoQueue/Inventory/Pool");
NO_QUEUE_BENCH(Bytes<Inventory>, "MessageNoQueue/Inventory/Bytes");

BENCHMARK(WriteOnly<Move>)->Name("MessageCodec/Move/Write");
BENCHMARK(ReadOnly<Move>)->Name("MessageCodec/Move/Read");
BENCHMARK(WriteOnly<Chat>)->Name("MessageCodec/Chat/Write");
BENCHMARK(ReadOnly<Chat>)->Name("MessageCodec/Chat/Read");
BENCHMARK(WriteOnly<Inventory>)->Name("MessageCodec/Inventory/Write");
BENCHMARK(ReadOnly<Inventory>)->Name("MessageCodec/Inventory/Read");
