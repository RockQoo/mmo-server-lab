#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Memory/ObjectPool.h"
#include "RentReturnGrid.h"
#include "ThreadTeam.h"

// 객체 풀 측정. 결과는 docs/journal/objectpool.html 에 정리한다. 여러 스레드는 ThreadTeam(미리 만든 스레드 N개가 같이
// 출발, C# Bench/ThreadTeam.cs 와 같음)으로 돌리고, 그 틀의 한계는 TeamNoop 으로 본다. volatile sink 는
// BufferPoolBench.cpp 주석 "DoNotOptimize" 와 같은 이유다.
//
// 1) SameThread: 객체 하나를 빌려 값을 쓰고 바로 반납한다. C# Bench/ObjectPoolBench 와 같은 일이다.
// 2) RentReturn: 빌리는 스레드 N개 → 반납하는 스레드 M개(64개씩 블록으로 넘김, Bench/RentReturnGrid.h). 2026-11-27
//    예전 CrossThread(독립된 1:1 쌍)를 이것으로 바꿨다. 예전 수치는 docs/journal/objectpool.html 에 남아 있다.
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
    constexpr int64_t TotalOps = int64_t{1} << 23;

    // ---- 0) 측정 틀의 한계: 아무 일도 하지 않는 반복(ThreadTeam)

    void TeamNoop(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        const auto ops = TotalOps / threads;
        Bench::ThreadTeam team(threads);
        for (auto iteration : state)
        {
            team.Run(
                [ops](int)
                {
                    volatile int64_t sink{};
                    for (auto i = int64_t{0}; i < ops; ++i)
                    {
                        sink = i;
                    }
                });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    struct Plain
    {
        uint64_t id{};
        void Reset() noexcept
        {
            id = 0;
        }
        void Use(const int64_t i)
        {
            id = static_cast<uint64_t>(i);
        }
    };

    struct WithList
    {
        uint64_t id{};
        std::vector<int> values;
        void Reset() noexcept
        {
            id = 0;
            values.clear();
        }
        void Use(const int64_t i)
        {
            id = static_cast<uint64_t>(i);
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
        const auto ops = TotalOps / threads;
        Bench::ThreadTeam team(threads);
        for (auto iteration : state)
        {
            team.Run(
                [ops](int)
                {
                    void* volatile sink{}; // 스레드마다 자기 변수(BufferPoolBench 주석 "DoNotOptimize")
                    for (auto i = int64_t{0}; i < ops; ++i)
                    {
                        auto object = Renter::Rent();
                        object->Use(i);
                        sink = object.get();
                    }
                });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    // ---- 2) 빌리는 스레드 N개 → 반납하는 스레드 M개(Bench/RentReturnGrid.h)

    template <typename Renter>
    void RentReturn(benchmark::State& state)
    {
        const auto renters = static_cast<int>(state.range(0));
        const auto returners = static_cast<int>(state.range(1));
        Bench::RentReturnGrid<typename Renter::Handle> grid(renters, returners); // 스레드는 측정 밖에서 한 번 만든다
        auto ops = int64_t{};                                                    // 한 횟수(= RentReturnOps)
        for (auto iteration : state)
        {
            ops = grid.Run(Bench::RentReturnOps,
                           []
                           {
                               auto object = Renter::Rent();
                               object->Use(1);
                               return object;
                           });
        }
        state.SetItemsProcessed(state.iterations() * ops);
    }

    // 측정 틀 자체의 비용(Codex 리뷰 R1): 빌리기·반납 대신 정수 하나를 같은 우편함으로 넘긴다. 이 값이 각 조합에서
    // 풀 측정보다 충분히 빨라야 풀 값을 풀 비용으로 읽을 수 있다. BufferPoolBench 도 같은 틀이라 이 값을 같이 쓴다.
    void RentReturnNoop(benchmark::State& state)
    {
        const auto renters = static_cast<int>(state.range(0));
        const auto returners = static_cast<int>(state.range(1));
        Bench::RentReturnGrid<uint64_t> grid(renters, returners);
        auto ops = int64_t{}; // 한 횟수(= RentReturnOps)
        for (auto iteration : state)
        {
            ops = grid.Run(Bench::RentReturnOps, [] { return uint64_t{1}; });
        }
        state.SetItemsProcessed(state.iterations() * ops);
    }
} // namespace

// 인자: 스레드 수
BENCHMARK(TeamNoop)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMillisecond);
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
// 인자: {빌리는 스레드 N, 반납하는 스레드 M}. 조합 목록은 Bench/RentReturnGrid.h 의 Bench::ApplyRentReturnShapes.
#define RENT_RETURN_BENCH(renter, name)                                                                                \
    BENCHMARK(RentReturn<renter>)                                                                                      \
        ->Name(name)                                                                                                   \
        ->Apply(Bench::ApplyRentReturnShapes)                                                                          \
        ->UseRealTime()                                                                                                \
        ->Unit(benchmark::kMillisecond)
BENCHMARK(RentReturnNoop)
    ->Name("RentReturnNoop")
    ->Apply(Bench::ApplyRentReturnShapes)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
RENT_RETURN_BENCH(NewDeleteRenter<Plain>, "ObjectRentReturn/NewDelete/Plain");
RENT_RETURN_BENCH(PoolRenter<Plain>, "ObjectRentReturn/ObjectPool/Plain");
RENT_RETURN_BENCH(NewDeleteRenter<WithList>, "ObjectRentReturn/NewDelete/WithList");
RENT_RETURN_BENCH(PoolRenter<WithList>, "ObjectRentReturn/ObjectPool/WithList");
