#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Memory/BufferPool.h"

// 버퍼 풀 측정 두 가지. 결과는 docs/journal/bufferpool.html, docs/decisions/ 에 정리한다.
//
// 1) SameThread: 버퍼 하나를 빌려 한 바이트 쓰고 바로 돌려준다. 같은 스레드가 빌리고 반납한다.
// 2) CrossThread: 스레드를 둘씩 짝짓는다. 짝수 스레드는 빌리기만 하고(Lane이 송신 패킷을 만드는 쪽), 홀수 스레드는
//    반납만 한다(IOCP 스레드가 송신 완료 후 버리는 쪽). 64개씩 묶어 넘긴다(넘기는 비용을 64개로 나눠 작게 한다).
//
// 측정 방법: 한 번 측정할 때마다 스레드 N개를 띄워 전체 TotalOps 번을 나눠 하고, 모두 끝날 때까지의 시간을 잰다.
// C# Bench/ArrayPoolBench(Parallel.For 로 전체 횟수를 나눔)와 같은 방법이다.
//
// 결과 해석: items_per_second = 모든 스레드를 합친 초당 (빌리기 + 반납) 횟수. 크면 좋다. 스레드 시작 비용은
// TotalOps 가 커서 전체의 몇 % 안 된다. Noop(아무 일도 하지 않는 반복)은 측정 틀의 기준이다: Noop 이 스레드 수만큼
// 늘지 않으면 측정 틀에 공유 자원이 있다는 뜻이라 여러 스레드 결과를 믿지 않는다.
//
// DoNotOptimize: 스레드 안 반복에서는 benchmark::DoNotOptimize 를 쓰지 않는다. MSVC 용 구현은 값을 전역 변수 하나에
// 쓰므로, 모든 스레드가 매 반복 같은 캐시 라인에 써서 서로 느려진다(2026-11-26: Noop 이 8스레드에서 1스레드와 같았다.
// 표준 C++만으로 재면 8배로 늘었다). 대신 스레드마다 자기 volatile 변수에 쓴다(docs/journal/bufferpool.html).
//
// 비교 대상
//   NewDelete     풀 없이 매번 new[]/delete[](기준)
//   BufferPoolV2  2판: 스레드 캐시 + 전역 풀(묶음). 1판(등급마다 mutex) 수치는 journal 에 남겼다(커밋 2209890 으로 다시
//   잴 수 있다)

namespace
{
    constexpr std::int64_t TotalOps = std::int64_t{1} << 23; // 약 840만. 1, 2, 4, 8 과 64(묶음)로 나누어떨어진다

    // 스레드 count개를 띄워 body(스레드 번호, 맡은 횟수)를 돌리고 모두 끝날 때까지 기다린다.
    template <typename Body>
    void RunOnThreads(const int count, const Body& body)
    {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<std::size_t>(count));
        for (auto t = 0; t < count; ++t)
        {
            threads.emplace_back([&body, t, count] { body(t, TotalOps / count); });
        }
    } // jthread 소멸 = join

    // ---- 빌리기 방법 두 가지를 같은 모양으로 감싼다

    // 풀 없이 new[]/delete[]. 소멸하면 delete[].
    class NewDeleteBuffer
    {
    public:
        NewDeleteBuffer() = default;
        explicit NewDeleteBuffer(const std::size_t size) : _data(std::make_unique_for_overwrite<std::byte[]>(size))
        {
        }
        ~NewDeleteBuffer() = default;
        NewDeleteBuffer(const NewDeleteBuffer&) = delete;
        NewDeleteBuffer& operator=(const NewDeleteBuffer&) = delete;
        NewDeleteBuffer(NewDeleteBuffer&&) noexcept = default;
        NewDeleteBuffer& operator=(NewDeleteBuffer&&) noexcept = default;

        [[nodiscard]] std::byte* Data() const noexcept
        {
            return _data.get();
        }
        void Reset() noexcept
        {
            _data.reset();
        }

    private:
        std::unique_ptr<std::byte[]> _data;
    };

    struct NewDeleteRenter
    {
        using Buffer = NewDeleteBuffer;
        static Buffer Rent(const std::size_t size)
        {
            return Buffer(size);
        }
    };

    struct PoolRenter
    {
        using Buffer = Memory::PooledBuffer;
        static Buffer Rent(const std::size_t size)
        {
            return Memory::BufferPool::Shared().Rent(size);
        }
    };

    // ---- 0) 측정 틀의 한계: 아무 일도 하지 않는 반복

    void Noop(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        for (auto iteration : state)
        {
            RunOnThreads(threads,
                         [](int, const std::int64_t ops)
                         {
                             volatile std::int64_t sink{}; // 스레드마다 자기 변수(아래 주석 "DoNotOptimize")
                             for (auto i = std::int64_t{0}; i < ops; ++i)
                             {
                                 sink = i;
                             }
                         });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    // ---- 1) 같은 스레드에서 빌리고 반납

    template <typename Renter>
    void SameThread(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        const auto size = static_cast<std::size_t>(state.range(1));
        for (auto iteration : state)
        {
            RunOnThreads(threads,
                         [size](int, const std::int64_t ops)
                         {
                             std::byte* volatile sink{}; // 스레드마다 자기 변수(아래 주석 "DoNotOptimize")
                             for (auto i = std::int64_t{0}; i < ops; ++i)
                             {
                                 auto buffer = Renter::Rent(size);
                                 buffer.Data()[0] = std::byte{1};
                                 sink = buffer.Data(); // 주소를 밖으로 내보내 컴파일러가 할당을 지우지 못하게 한다
                             }
                         });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    // ---- 2) 빌리는 스레드와 반납하는 스레드가 다르다

    constexpr auto BlockSize = 64;

    template <typename Renter>
    struct Pair
    {
        using Block = std::array<typename Renter::Buffer, BlockSize>;

        Pair() = default;
        ~Pair() = default;
        Pair(const Pair&) = delete; // atomic 을 들고 있어 옮길 수 없다
        Pair& operator=(const Pair&) = delete;
        Pair(Pair&&) = delete;
        Pair& operator=(Pair&&) = delete;

        std::array<Block, 2> blocks{}; // 빌리는 쪽이 번갈아 채운다
        std::atomic<Block*> mailbox{}; // 비었으면 nullptr
    };

    // 빌리는 쪽: 블록 하나를 채우고, 우편함이 비면(반납하는 쪽이 이전 블록을 다 버리면) 넣는다.
    // 우편함이 비었다는 것은 이전에 넣은 블록을 다 버렸다는 뜻이라, 블록 두 개를 번갈아 쓰면 안전하다.
    template <typename Renter>
    void Producer(Pair<Renter>& pair, const std::size_t size, const std::int64_t ops)
    {
        auto next = std::size_t{0};
        for (auto done = std::int64_t{0}; done < ops; done += BlockSize)
        {
            auto& block = pair.blocks[next];
            for (auto& buffer : block)
            {
                buffer = Renter::Rent(size);
                buffer.Data()[0] = std::byte{1};
            }
            while (pair.mailbox.load(std::memory_order_acquire) != nullptr)
            {
                std::this_thread::yield();
            }
            pair.mailbox.store(&block, std::memory_order_release);
            next ^= 1;
        }
    }

    // 반납하는 쪽: 우편함의 블록을 받아 모두 버리고 우편함을 비운다.
    template <typename Renter>
    void Consumer(Pair<Renter>& pair, const std::int64_t ops)
    {
        for (auto done = std::int64_t{0}; done < ops; done += BlockSize)
        {
            typename Pair<Renter>::Block* block{};
            while ((block = pair.mailbox.load(std::memory_order_acquire)) == nullptr)
            {
                std::this_thread::yield();
            }
            for (auto& buffer : *block)
            {
                buffer.Reset();
            }
            pair.mailbox.store(nullptr, std::memory_order_release);
        }
    }

    template <typename Renter>
    void CrossThread(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        const auto size = static_cast<std::size_t>(state.range(1));
        for (auto iteration : state)
        {
            const auto pairs = std::make_unique<Pair<Renter>[]>(static_cast<std::size_t>(threads / 2));
            // 스레드마다 TotalOps/threads 를 받는다. 짝 하나(빌리기 + 반납)가 2배를 맡아야 전체가 TotalOps 다.
            RunOnThreads(threads,
                         [&pairs, size](const int t, const std::int64_t ops)
                         {
                             auto& pair = pairs[static_cast<std::size_t>(t / 2)];
                             if (t % 2 == 0)
                             {
                                 Producer(pair, size, ops * 2);
                             }
                             else
                             {
                                 Consumer(pair, ops * 2);
                             }
                         });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }
} // namespace

// 인자: {스레드 수, 크기}. 크기: 256B(이동 패킷), 4KB(큰 패킷).
BENCHMARK(Noop)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<NewDeleteRenter>)
    ->Name("SameThread/NewDelete")
    ->ArgsProduct({{1, 2, 4, 8}, {256, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<PoolRenter>)
    ->Name("SameThread/BufferPoolV2")
    ->ArgsProduct({{1, 2, 4, 8}, {256, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
// 짝이 맞아야 해서 스레드 2, 4, 8. 크기마다 캐시 개수·묶음이 달라(256B·1KB 64·32, 4KB 16·8) 셋 다 잰다.
BENCHMARK(CrossThread<NewDeleteRenter>)
    ->Name("CrossThread/NewDelete")
    ->ArgsProduct({{2, 4, 8}, {256, 1024, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(CrossThread<PoolRenter>)
    ->Name("CrossThread/BufferPoolV2")
    ->ArgsProduct({{2, 4, 8}, {256, 1024, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
