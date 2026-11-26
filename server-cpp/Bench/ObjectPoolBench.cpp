#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Memory/ObjectPool.h"

// 객체 풀 측정. 결과는 docs/journal/objectpool.html 에 정리한다. 측정 틀(스레드 나누기, volatile sink, Noop 기준)은
// BufferPoolBench.cpp 와 같다(Noop 도 그 파일에 있다).
//
// 1) SameThread: 객체 하나를 빌려 값을 쓰고 바로 반납한다. C# Bench/ObjectPoolBench 와 같은 일이다.
// 2) CrossThread: 빌리는 스레드와 반납하는 스레드가 다르다(64개씩 묶어 넘김, BufferPoolBench 와 같은 모양).
//
// 객체 두 가지
//   Plain     값만 있다(id 하나). 풀 자체의 비용을 본다
//   WithList  vector<int> 가 있고 하나를 넣는다. new/delete 는 매번 vector 할당도 하고, 풀은 Reset()이 clear()라 용량을
//             다시 쓴다(반납할 때 소멸하지 않는 방식을 고른 이유)
//
// 비교 대상
//   NewDelete   풀 없이 매번 make_unique(기준)
//   ObjectPool  스레드 캐시 + 전역 묶음

namespace
{
    constexpr std::int64_t TotalOps = std::int64_t{1} << 23;

    template <typename Body>
    void RunOnThreads(const int count, const Body& body)
    {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<std::size_t>(count));
        for (auto t = 0; t < count; ++t)
        {
            threads.emplace_back([&body, count] { body(TotalOps / count); });
        }
    }

    struct Plain
    {
        std::uint64_t id{};
        void Reset() noexcept
        {
            id = 0;
        }
        void Use(const std::int64_t i)
        {
            id = static_cast<std::uint64_t>(i);
        }
    };

    struct WithList
    {
        std::uint64_t id{};
        std::vector<int> values;
        void Reset() noexcept
        {
            id = 0;
            values.clear();
        }
        void Use(const std::int64_t i)
        {
            id = static_cast<std::uint64_t>(i);
            values.push_back(1);
        }
    };

    template <typename T>
    struct NewDeleteRenter
    {
        using Handle = std::unique_ptr<T>;
        static Handle Rent()
        {
            return std::make_unique<T>();
        }
    };

    template <typename T>
    struct PoolRenter
    {
        using Handle = Memory::PooledPtr<T>;
        static Handle Rent()
        {
            return Memory::ObjectPool<T>::Shared().Rent();
        }
    };

    // ---- 1) 같은 스레드에서 빌리고 반납

    template <typename Renter>
    void SameThread(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        for (auto iteration : state)
        {
            RunOnThreads(threads,
                         [](const std::int64_t ops)
                         {
                             void* volatile sink{}; // 스레드마다 자기 변수(BufferPoolBench 주석 "DoNotOptimize")
                             for (auto i = std::int64_t{0}; i < ops; ++i)
                             {
                                 auto object = Renter::Rent();
                                 object->Use(i);
                                 sink = object.get();
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
        using Block = std::array<typename Renter::Handle, BlockSize>;

        Pair() = default;
        ~Pair() = default;
        Pair(const Pair&) = delete;
        Pair& operator=(const Pair&) = delete;
        Pair(Pair&&) = delete;
        Pair& operator=(Pair&&) = delete;

        std::array<Block, 2> blocks{};
        std::atomic<Block*> mailbox{};
    };

    template <typename Renter>
    void Producer(Pair<Renter>& pair, const std::int64_t ops)
    {
        auto next = std::size_t{0};
        for (auto done = std::int64_t{0}; done < ops; done += BlockSize)
        {
            auto& block = pair.blocks[next];
            for (auto& object : block)
            {
                object = Renter::Rent();
                object->Use(done);
            }
            while (pair.mailbox.load(std::memory_order_acquire) != nullptr)
            {
                std::this_thread::yield();
            }
            pair.mailbox.store(&block, std::memory_order_release);
            next ^= 1;
        }
    }

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
            for (auto& object : *block)
            {
                object.reset();
            }
            pair.mailbox.store(nullptr, std::memory_order_release);
        }
    }

    template <typename Renter>
    void CrossThread(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        for (auto iteration : state)
        {
            const auto pairs = std::make_unique<Pair<Renter>[]>(static_cast<std::size_t>(threads / 2));
            std::atomic<int> nextIndex{};
            RunOnThreads(threads,
                         [&pairs, &nextIndex](const std::int64_t ops)
                         {
                             const auto t = nextIndex.fetch_add(1);
                             auto& pair = pairs[static_cast<std::size_t>(t / 2)];
                             if (t % 2 == 0)
                             {
                                 Producer(pair, ops * 2);
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

// 인자: 스레드 수
BENCHMARK(SameThread<NewDeleteRenter<Plain>>)
    ->Name("ObjectSameThread/NewDelete/Plain")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<PoolRenter<Plain>>)
    ->Name("ObjectSameThread/ObjectPool/Plain")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<NewDeleteRenter<WithList>>)
    ->Name("ObjectSameThread/NewDelete/WithList")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<PoolRenter<WithList>>)
    ->Name("ObjectSameThread/ObjectPool/WithList")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(CrossThread<NewDeleteRenter<WithList>>)
    ->Name("ObjectCrossThread/NewDelete/WithList")
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(CrossThread<PoolRenter<WithList>>)
    ->Name("ObjectCrossThread/ObjectPool/WithList")
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
