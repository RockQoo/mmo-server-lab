#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "Metrics/LatencyHistogram.h"
#include "Metrics/MetricCounter.h"

// 등록한 지표를 로그 한 줄 글로 만든다. 서버가 Scheduler 로 10초마다 불러 로그에 남긴다.
//
//     Metrics::MetricsReporter reporter;
//     reporter.Add("tick.latency", tickLatency);
//     reporter.Add("packets.received", received);
//     scheduler.Every(10s, zoneLane, [&reporter] { Log::Info(LogCategory::Metrics, reporter.Report()); });
//     → tick.latency=[n=600 p50=812.0us p99=3.10ms max=12.00ms] packets.received=1200/s
//
// - 히스토그램은 SnapshotAndReset: 지난 Report 뒤 구간의 분포다(n=0 이면 그 구간에 기록이 없었다).
// - 카운터는 지난 Report 뒤 늘어난 양을 그 사이 시간으로 나눈 초당 개수다. Add 하기 전에 늘어난 양은 세지 않는다.
// - 글만 만들고 Log 는 모른다. 카테고리(게임 레이어 enum)와 출력은 서버가 정한다.
//   그래서 Metrics 는 Log 에 기대지 않는다.
// - 등록한 순서대로 쓴다. Add 는 Report 를 시작하기 전에 끝낸다. Report 는 한 곳(Lane 하나)에서만 부른다.
// - 등록한 지표는 reporter 보다 오래 살아야 한다(가리키기만 한다).
namespace Metrics
{
    class MetricsReporter
    {
    public:
        using Clock = std::chrono::steady_clock;

        // start: 첫 Report 의 초당 개수를 낼 때 시작 시각(테스트는 직접 준다)
        explicit MetricsReporter(Clock::time_point start = Clock::now());

        void Add(std::string name, const MetricCounter& counter);
        void Add(std::string name, LatencyHistogram& histogram);

        // 지난 Report(처음이면 만든 때) 뒤 구간을 한 줄로. now 는 테스트가 직접 준다.
        [[nodiscard]] std::string Report(Clock::time_point now = Clock::now());

        // ns 를 읽기 쉬운 단위로: 850ns, 12.3us, 3.10ms, 1.20s
        [[nodiscard]] static std::string FormatDuration(int64_t nanoseconds);

    private:
        // 둘 중 하나만 있다
        struct Entry
        {
            std::string name;
            const MetricCounter* counter{};
            LatencyHistogram* histogram{};
            int64_t lastValue{}; // 카운터의 지난 Report 때 값
        };

        std::vector<Entry> _entries{};
        Clock::time_point _lastReport;
    };
} // namespace Metrics
