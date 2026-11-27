#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "Metrics/MetricCounter.h"
#include "ThreadTeam.h"

// 카운터 측정: atomic 하나(SingleAtomic) vs 칸 나누기(MetricCounter). 결과는 docs/journal/performance.html.
// 여러 스레드 규칙(CLAUDE.md, docs/journal/benchmark.html):
//   - TeamNoop 을 같은 스레드 수로 같이 잰다. 스레드 수만큼 늘지 않으면 그 결과를 믿지 않는다.
//   - 스레드는 ThreadTeam 으로 미리 만들고 같이 출발한다. DoNotOptimize 는 반복 안에서 쓰지 않는다.
//   - 1:1·N:1 만으로 끝내지 않고 N:M 을 같이 잰다: 스레드 N 개가 카운터 M 개를 번갈아 올린다(서버에서 IOCP 스레드
//     여럿이 "받은 패킷 수", "받은 바이트", "보낸 패킷 수"… 를 같이 올리는 모양).
// 측정 범위는 Add 만이다(Value 는 10초에 한 번 읽으므로 재지 않는다). 칸 번호를 정하는 첫 Add 는 출발 전에 한 번 한다.

namespace
{
    constexpr int64_t TotalOps = int64_t{1} << 24;

    // 비교 기준: 칸 없이 atomic 하나. 카운터끼리 같은 캐시 줄에 놓이지 않게 하나씩 힙에 띄운다.
    struct SingleAtomic
    {
        SingleAtomic() = default;
        ~SingleAtomic() = default;
        SingleAtomic(const SingleAtomic&) = delete;
        SingleAtomic& operator=(const SingleAtomic&) = delete;
        SingleAtomic(SingleAtomic&&) = delete;
        SingleAtomic& operator=(SingleAtomic&&) = delete;

        std::atomic<int64_t> value{};
        std::array<std::byte, 64 - sizeof(std::atomic<int64_t>)> padding{};

        void Add(const int64_t delta = 1) noexcept
        {
            value.fetch_add(delta, std::memory_order_relaxed);
        }
    };

    void TeamNoop(benchmark::State& state)
    {
        const auto threads = static_cast<int32_t>(state.range(0));
        const auto ops = TotalOps / threads;
        Bench::ThreadTeam team(threads);
        for (auto iteration : state)
        {
            team.Run(
                [ops](int32_t)
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

    // range(0) = 스레드 N, range(1) = 카운터 M. 스레드마다 시작 카운터를 달리해 M 개를 번갈아 올린다.
    template <typename Counter>
    void AddMany(benchmark::State& state)
    {
        const auto threads = static_cast<int32_t>(state.range(0));
        const auto counters = static_cast<int32_t>(state.range(1));
        const auto ops = TotalOps / threads;
        auto storage = std::make_unique<Counter[]>(static_cast<std::size_t>(counters));
        Bench::ThreadTeam team(threads);
        team.Run([&](int32_t) { storage[0].Add(0); }); // 칸 번호 정하기(측정 밖)
        for (auto iteration : state)
        {
            team.Run(
                [&storage, ops, counters](const int32_t t)
                {
                    auto index = t % counters;
                    for (auto i = int64_t{0}; i < ops; ++i)
                    {
                        storage[static_cast<std::size_t>(index)].Add();
                        index = index + 1 == counters ? 0 : index + 1;
                    }
                });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    void ThreadArgs(benchmark::Benchmark* bench)
    {
        for (const auto threads : {1, 2, 4, 8, 16})
        {
            bench->Arg(threads);
        }
    }

    // N:1(카운터 하나에 모두) 과 N:M(카운터 4개를 섞어서)
    void CounterArgs(benchmark::Benchmark* bench)
    {
        for (const auto counters : {1, 4})
        {
            for (const auto threads : {1, 2, 4, 8, 16})
            {
                bench->Args({threads, counters});
            }
        }
    }
} // namespace

BENCHMARK(TeamNoop)->Apply(ThreadArgs)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(AddMany<SingleAtomic>)->Apply(CounterArgs)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(AddMany<Metrics::MetricCounter>)->Apply(CounterArgs)->UseRealTime()->Unit(benchmark::kMillisecond);
