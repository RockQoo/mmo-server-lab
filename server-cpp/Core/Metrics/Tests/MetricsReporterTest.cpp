#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include "Metrics/LatencyHistogram.h"
#include "Metrics/MetricCounter.h"
#include "Metrics/MetricsReporter.h"

using namespace std::chrono_literals;
using Metrics::MetricsReporter;

TEST(MetricsReporter, FormatsDurationInReadableUnit)
{
    EXPECT_EQ(MetricsReporter::FormatDuration(850), "850ns");
    EXPECT_EQ(MetricsReporter::FormatDuration(12'300), "12.3us");
    EXPECT_EQ(MetricsReporter::FormatDuration(3'100'000), "3.10ms");
    EXPECT_EQ(MetricsReporter::FormatDuration(1'200'000'000), "1.20s");
}

// 카운터는 구간마다 초당 개수, 히스토그램은 구간마다 분포. 등록한 순서대로 쓴다.
TEST(MetricsReporter, ReportsEachIntervalInRegistrationOrder)
{
    const auto start = MetricsReporter::Clock::time_point{};
    Metrics::LatencyHistogram tick;
    Metrics::MetricCounter received;
    received.Add(999); // 등록 전에 늘어난 양은 세지 않는다

    MetricsReporter reporter;
    reporter.Add("tick", tick);
    reporter.Add("received", received, start);

    for (auto i = int32_t{0}; i < 4; ++i)
    {
        tick.Record(1ms);
    }
    received.Add(100);
    EXPECT_EQ(reporter.Report(start + 10s), "tick=[n=4 p50=1.00ms p99=1.00ms max=1.00ms] received=10/s");
    // 다음 구간: 아무것도 없었다
    EXPECT_EQ(reporter.Report(start + 20s), "tick=[n=0] received=0/s");
}

// 늦게 Add 한 카운터는 자기 Add 시각부터 센다(Codex 리뷰 R1: 공통 시작 시각이면 첫 초당 개수가 작게 나왔다)
TEST(MetricsReporter, LateCounterStartsFromItsOwnAdd)
{
    const auto start = MetricsReporter::Clock::time_point{};
    Metrics::MetricCounter early;
    Metrics::MetricCounter late;
    MetricsReporter reporter;
    reporter.Add("early", early, start);
    reporter.Add("late", late, start + 5s);

    early.Add(100);
    late.Add(100);
    EXPECT_EQ(reporter.Report(start + 10s), "early=10/s late=20/s");
}
