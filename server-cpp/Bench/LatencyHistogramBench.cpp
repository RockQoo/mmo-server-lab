#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>

#include "Metrics/LatencyHistogram.h"
#include "Metrics/ScopedTimer.h"
#include "ThreadTeam.h"

// 지연 측정 비용(PROGRESS "측정 비용"). 결과는 docs/journal/performance.html.
//   SteadyClockNow    시계를 한 번 읽는 비용. ScopedTimer 는 두 번 읽는다
//   Record            값 하나 넣기(칸 번호 + atomic 더하기 셋)
//   ScopedTimerEmpty  빈 블록을 잰다 = 시계 두 번 + Record. 서버가 틱·패킷마다 내는 실제 비용
//   RecordShared      스레드 N 개가 히스토그램 하나에 같이 넣는다(같은 칸·합·max 를 두고 다툰다)
// 여러 스레드 기준선은 MetricCounterBench.cpp 의 TeamNoop(같은 ThreadTeam, 같은 스레드 수)이다.

namespace
{
    constexpr int64_t TotalOps = int64_t{1} << 22;

    void SteadyClockNow(benchmark::State& state)
    {
        for (auto iteration : state)
        {
            benchmark::DoNotOptimize(std::chrono::steady_clock::now());
        }
    }

    void Record(benchmark::State& state)
    {
        Metrics::LatencyHistogram histogram;
        auto value = int64_t{1000};
        for (auto iteration : state)
        {
            histogram.Record(std::chrono::nanoseconds{value});
            value = value < 50'000'000 ? value * 3 / 2 : 1000; // 1µs~50ms 를 돌며 여러 칸을 쓴다
        }
        benchmark::DoNotOptimize(histogram.Snapshot().count);
    }

    void ScopedTimerEmpty(benchmark::State& state)
    {
        Metrics::LatencyHistogram histogram;
        for (auto iteration : state)
        {
            const Metrics::ScopedTimer timer{histogram};
        }
        benchmark::DoNotOptimize(histogram.Snapshot().count);
    }

    // 스레드마다 다른 값(다른 칸)을 넣지만 합·max 는 모두 같은 줄이다
    void RecordShared(benchmark::State& state)
    {
        const auto threads = static_cast<int32_t>(state.range(0));
        const auto ops = TotalOps / threads;
        Metrics::LatencyHistogram histogram;
        Bench::ThreadTeam team(threads);
        for (auto iteration : state)
        {
            team.Run(
                [&histogram, ops](const int32_t t)
                {
                    auto value = int64_t{1000} + t;
                    for (auto i = int64_t{0}; i < ops; ++i)
                    {
                        histogram.Record(std::chrono::nanoseconds{value});
                        value = value < 50'000'000 ? value * 3 / 2 : int64_t{1000} + t;
                    }
                });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }
} // namespace

BENCHMARK(SteadyClockNow);
BENCHMARK(Record);
BENCHMARK(ScopedTimerEmpty);
BENCHMARK(RecordShared)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->UseRealTime()->Unit(benchmark::kMillisecond);
