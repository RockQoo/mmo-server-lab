#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "Log/Log.h"
#include "ThreadTeam.h"

// 로그 한 줄 비용. C# Bench/LogBench.cs(Serilog + Sinks.Async)와 같은 줄, 같은 횟수, 같은 스레드 수.
// 결과는 docs/journal/performance.html 사례(로그).
//
// 스레드 T개가 모두 합쳐 TotalLines 줄을 찍는다:
//   Log::Info(BenchCategory::Battle, "skill hit").KV("attacker", i).KV("damage", 350)
// 잰 값 두 가지:
//   - 시간(Time, manual): 첫 줄을 넣기 시작해 마지막 줄이 sink 까지 써질 때까지(Flush). 로그 Lane 하나가 다 쓰는 속도라
//     스레드를 늘려도 줄지 않는다(소비자 하나).
//   - caller_ns: 게임 스레드가 한 줄에 쓴 시간(넣기만, 스레드마다 평균). 게임 스레드가 실제로 잃는 시간이다.
// sink: Null(버림, 로그 틀 자체 비용) / File(날짜별 파일, 줄마다 flush, 실제 모양).
namespace
{
    constexpr int64_t TotalLines = int64_t{1} << 16; // 65,536. 1·4·8 로 나누어떨어진다

    enum class BenchCategory : uint8_t
    {
        System,
        Battle,
    };

    class NullSink final : public Log::LogSink
    {
    public:
        void Write(const Log::LogEvent&, std::string_view) override
        {
        }
    };

    template <bool ToFile>
    void LogLine(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        const auto dir = std::filesystem::temp_directory_path() / "mmo-server-lab-LogBench";
        std::vector<std::unique_ptr<Log::LogSink>> sinks;
        if constexpr (ToFile)
        {
            std::filesystem::remove_all(dir);
            sinks.push_back(std::make_unique<Log::FileSink>(dir / "bench-.log"));
        }
        else
        {
            sinks.push_back(std::make_unique<NullSink>());
        }
        Log::Initialize({.minimumLevel = Log::Level::Info}, std::move(sinks));

        Bench::ThreadTeam team(threads);
        const auto perThread = TotalLines / threads;
        auto callerNs = 0.0;
        for (auto iteration : state)
        {
            const auto start = std::chrono::steady_clock::now();
            team.Run(
                [perThread](int)
                {
                    for (auto i = int64_t{0}; i < perThread; ++i)
                    {
                        Log::Info(BenchCategory::Battle, "skill hit").KV("attacker", i).KV("damage", 350);
                    }
                });
            const auto produced = std::chrono::steady_clock::now();
            Log::Flush();
            const auto written = std::chrono::steady_clock::now();
            state.SetIterationTime(std::chrono::duration<double>(written - start).count());
            // 모든 스레드가 같이 출발하고 같이 끝나므로 넣기 끝난 시각 / 스레드당 줄 수 = 스레드 하나가 한 줄에 쓴 시간
            callerNs +=
                std::chrono::duration<double, std::nano>(produced - start).count() / static_cast<double>(perThread);
        }
        Log::Shutdown();
        if constexpr (ToFile)
        {
            std::filesystem::remove_all(dir);
        }
        state.SetItemsProcessed(state.iterations() * TotalLines);
        state.counters["caller_ns"] = callerNs / static_cast<double>(state.iterations());
    }
} // namespace

BENCHMARK(LogLine<false>)
    ->Name("LogLine/Null")
    ->ArgName("threads")
    ->Arg(1)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(LogLine<true>)
    ->Name("LogLine/File")
    ->ArgName("threads")
    ->Arg(1)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
