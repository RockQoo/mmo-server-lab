#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "Scheduling/TimerHeap.h"
#include "TimerWheel.h"

// 타이머 측정: Scheduling::TimerHeap(priority_queue + unordered_map, 지우기는 표시만) vs 비교용 TimerWheel(한 단계 해시
// 휠, Bench/TimerWheel.h). PROGRESS.md Timer 줄의 "우선순위 큐 vs 타이머 휠"을 수치로 정한다.
//
// 한 스레드 측정이다. TimerHeap 은 한 스레드(존 스레드나 타이머 스레드)만 쓰는 구조라 여러 스레드 규칙(Noop 1·2·4·8,
// N:M)은 해당하지 않는다. 그래서 benchmark::DoNotOptimize 도 그대로 쓴다(true sharing 문제는 여러 스레드에서만 생긴다).
//
// 측정 네 가지(N = 10,000 / 100,000 / 1,000,000). items_per_second = 초당 연산 수, ns/연산 = 1e9 / items_per_second.
//   Add     빈 구조에 N개 넣기. 시각은 1ms~60초에 고르게(정해진 seed)
//   Cancel  N개를 넣어 두고(시간 밖) 섞은 순서로 모두 지우기(버프 해제)
//   RunDue  1ms~10초에 퍼진 N개를 1ms씩 시각을 올리며 모두 실행(실행한 일 하나당 비용)
//   Mixed   존 서버 흉내: 긴 버프 0.9N(30초~30분, 대부분 해제됨) + 1초 주기 도트 0.1N. 50ms 틱으로 10초 동안 틱마다
//           버프 N/1000개 추가·N/1000개 해제, 도트 실행. 연산 = 추가 + 해제 + 실행
// 구조를 만들고 부수는 것은 시간 밖(PauseTiming)이다. N 이 1만 이상이라 PauseTiming 비용은 묻힌다.
//
// 휠 설정: 칸 1ms × 4096개(한 바퀴 약 4.1초). 한 바퀴보다 먼 일은 바퀴마다 한 번씩 훑인다(Mixed 에서 그 비용이 보인다).

namespace
{
    using namespace std::chrono_literals;

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;
    using Job = Scheduling::TimerHeap::Job;

    const auto Start = TimePoint{};
    constexpr auto WheelTick = 1ms;
    constexpr auto WheelSlots = std::size_t{4096};
    constexpr auto Seed = uint64_t{20261128};

    // 정해진 seed 의 난수: 매번 같은 시각·순서로 재야 두 구조와 여러 번의 측정을 비교할 수 있다
    std::mt19937_64 MakeRandom(const uint64_t stream)
    {
        return std::mt19937_64{Seed + stream}; // NOLINT(bugprone-random-generator-seed): 일부러 같은 수열
    }

    // 두 구조를 같은 모양으로 감싼다. id 는 uint64_t 로 맞춘다.
    struct HeapTimers
    {
        Scheduling::TimerHeap heap;

        uint64_t Add(const TimePoint due, Job job, const Duration period = Duration::zero())
        {
            return heap.Add(due, std::move(job), period).Value();
        }
        bool Cancel(const uint64_t id)
        {
            return heap.Cancel(Scheduling::TimerId{id});
        }
        std::size_t RunDue(const TimePoint now)
        {
            return heap.RunDue(now);
        }
    };

    struct WheelTimers
    {
        Bench::TimerWheel wheel = Bench::TimerWheel(Start, WheelTick, WheelSlots);

        uint64_t Add(const TimePoint due, Job job, const Duration period = Duration::zero())
        {
            return wheel.Add(due, std::move(job), period);
        }
        bool Cancel(const uint64_t id)
        {
            return wheel.Cancel(id);
        }
        std::size_t RunDue(const TimePoint now)
        {
            return wheel.RunDue(now);
        }
    };

    // [minMs, maxMs] 에 고르게 퍼진 시각 count 개(정해진 seed 라 매번 같다)
    std::vector<TimePoint> SpreadDues(const int64_t count, const int64_t minMs, const int64_t maxMs)
    {
        auto random = MakeRandom(0);
        std::uniform_int_distribution<int64_t> ms{minMs, maxMs};
        std::vector<TimePoint> dues;
        dues.reserve(static_cast<std::size_t>(count));
        for (auto i = int64_t{0}; i < count; ++i)
        {
            dues.push_back(Start + std::chrono::milliseconds{ms(random)});
        }
        return dues;
    }

    // ---- Add

    template <typename Timers>
    void Add(benchmark::State& state)
    {
        const auto count = state.range(0);
        const auto dues = SpreadDues(count, 1, 60'000);
        auto fired = int64_t{0};
        for (auto iteration : state)
        {
            state.PauseTiming();
            auto timers = std::make_unique<Timers>();
            state.ResumeTiming();
            for (const auto due : dues)
            {
                timers->Add(due, [&fired] { ++fired; });
            }
            state.PauseTiming();
            timers.reset();
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(fired);
        state.SetItemsProcessed(state.iterations() * count);
    }

    // ---- Cancel

    template <typename Timers>
    void Cancel(benchmark::State& state)
    {
        const auto count = state.range(0);
        const auto dues = SpreadDues(count, 1, 60'000);
        std::vector<std::size_t> order(static_cast<std::size_t>(count));
        for (auto i = std::size_t{0}; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::ranges::shuffle(order, MakeRandom(1));
        auto fired = int64_t{0};
        auto canceled = int64_t{0};
        std::vector<uint64_t> ids;
        ids.reserve(order.size());
        for (auto iteration : state)
        {
            state.PauseTiming();
            auto timers = std::make_unique<Timers>();
            ids.clear();
            for (const auto due : dues)
            {
                ids.push_back(timers->Add(due, [&fired] { ++fired; }));
            }
            state.ResumeTiming();
            for (const auto index : order)
            {
                canceled += timers->Cancel(ids[index]) ? 1 : 0;
            }
            state.PauseTiming();
            timers.reset();
            state.ResumeTiming();
        }
        if (canceled != state.iterations() * count)
        {
            state.SkipWithError("Cancel: not every timer was canceled");
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

    // ---- RunDue

    template <typename Timers>
    void RunDue(benchmark::State& state)
    {
        const auto count = state.range(0);
        constexpr auto SpanMs = int64_t{10'000};
        const auto dues = SpreadDues(count, 1, SpanMs);
        auto fired = int64_t{0};
        for (auto iteration : state)
        {
            state.PauseTiming();
            auto timers = std::make_unique<Timers>();
            for (const auto due : dues)
            {
                timers->Add(due, [&fired] { ++fired; });
            }
            state.ResumeTiming();
            for (auto ms = int64_t{1}; ms <= SpanMs; ++ms)
            {
                timers->RunDue(Start + std::chrono::milliseconds{ms});
            }
            state.PauseTiming();
            timers.reset();
            state.ResumeTiming();
        }
        if (fired != state.iterations() * count)
        {
            state.SkipWithError("RunDue: not every timer fired");
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

    // ---- Mixed: 존 서버 흉내

    template <typename Timers>
    void Mixed(benchmark::State& state)
    {
        const auto count = state.range(0);
        const auto buffs = count * 9 / 10;
        const auto dots = count - buffs;
        constexpr auto ZoneTick = 50ms;
        constexpr auto Ticks = int64_t{200};                   // 10초
        const auto churn = std::max(count / 1000, int64_t{1}); // 틱마다 추가·해제하는 버프 수

        // 난수는 모두 미리 만든다(시간 안에 난수 비용이 섞이지 않게)
        const auto buffDues = SpreadDues(buffs, 30'000, 1'800'000);
        const auto dotDues = SpreadDues(dots, 1, 1'000);
        const auto newBuffDues = SpreadDues(Ticks * churn, 30'000, 1'800'000); // 그 틱 시각에 더한다
        std::vector<uint32_t> cancelPicks(static_cast<std::size_t>(Ticks * churn));
        auto random = MakeRandom(2);
        for (auto& pick : cancelPicks)
        {
            pick = static_cast<uint32_t>(random());
        }

        auto fired = int64_t{0};
        auto ops = int64_t{0};
        std::vector<uint64_t> live;
        for (auto iteration : state)
        {
            state.PauseTiming();
            auto timers = std::make_unique<Timers>();
            live.clear();
            for (const auto due : buffDues)
            {
                live.push_back(timers->Add(due, [&fired] { ++fired; }));
            }
            for (const auto due : dotDues)
            {
                timers->Add(due, [&fired] { ++fired; }, 1s);
            }
            state.ResumeTiming();
            auto next = std::size_t{0};
            for (auto tick = int64_t{1}; tick <= Ticks; ++tick)
            {
                const auto now = Start + tick * ZoneTick;
                for (auto i = int64_t{0}; i < churn; ++i, ++next)
                {
                    live.push_back(timers->Add(now + (newBuffDues[next] - Start), [&fired] { ++fired; }));
                    const auto pick = cancelPicks[next] % live.size();
                    timers->Cancel(live[pick]);
                    live[pick] = live.back();
                    live.pop_back();
                }
                ops += 2 * churn + static_cast<int64_t>(timers->RunDue(now));
            }
            state.PauseTiming();
            timers.reset();
            state.ResumeTiming();
        }
        // 버프는 30초 뒤라 10초 안에 실행되지 않고, 도트는 1초 안에 시작해 1초마다라 10번씩 실행된다
        if (fired != state.iterations() * dots * 10)
        {
            state.SkipWithError("Mixed: unexpected fire count");
        }
        state.SetItemsProcessed(ops);
    }

    // N 세 가지. 한 번에 수십 ms 걸리므로 ms 로 보인다
    void Sizes(benchmark::Benchmark* bench)
    {
        bench->Arg(10'000)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMillisecond);
    }

    BENCHMARK(Add<HeapTimers>)->Apply(Sizes);
    BENCHMARK(Add<WheelTimers>)->Apply(Sizes);
    BENCHMARK(Cancel<HeapTimers>)->Apply(Sizes);
    BENCHMARK(Cancel<WheelTimers>)->Apply(Sizes);
    BENCHMARK(RunDue<HeapTimers>)->Apply(Sizes);
    BENCHMARK(RunDue<WheelTimers>)->Apply(Sizes);
    BENCHMARK(Mixed<HeapTimers>)->Apply(Sizes);
    BENCHMARK(Mixed<WheelTimers>)->Apply(Sizes);
} // namespace
