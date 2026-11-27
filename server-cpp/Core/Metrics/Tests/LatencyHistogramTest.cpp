#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "Metrics/LatencyHistogram.h"
#include "Metrics/ScopedTimer.h"

using namespace std::chrono_literals;
using Metrics::LatencyHistogram;

TEST(LatencyHistogram, EmptySnapshotIsAllZero)
{
    LatencyHistogram histogram;
    const auto snapshot = histogram.Snapshot();

    EXPECT_EQ(snapshot.count, 0);
    EXPECT_EQ(snapshot.p50, 0);
    EXPECT_EQ(snapshot.p99, 0);
    EXPECT_EQ(snapshot.max, 0);
}

// 모든 값이 자기 칸 안에 들어가고, 칸 폭이 값의 1/16 이하(오차 6.25% 이하)이며, 칸 번호가 값 순서대로 늘어난다
TEST(LatencyHistogram, BucketsCoverValuesWithinSixPercent)
{
    auto previous = std::size_t{0};
    for (auto value = uint64_t{0}; value < (uint64_t{1} << 36); value = value < 64 ? value + 1 : value + value / 7)
    {
        const auto bucket = LatencyHistogram::BucketOf(value);
        ASSERT_LT(bucket, LatencyHistogram::BucketCount);
        ASSERT_GE(bucket, previous) << value;
        const auto upper = LatencyHistogram::UpperBoundOf(bucket);
        ASSERT_GE(upper, static_cast<int64_t>(value)) << value;
        ASSERT_LE(upper - static_cast<int64_t>(value), static_cast<int64_t>(value) / 16) << value;
        previous = bucket;
    }
    EXPECT_EQ(LatencyHistogram::BucketOf(uint64_t{1} << 40), LatencyHistogram::BucketCount - 1); // 넘으면 마지막 칸
}

// C# LatencyHistogramTest.BucketNumbersMatchCpp 와 같은 값 → 같은 칸(두 언어의 p99 가 같은 값이 되는 근거)
TEST(LatencyHistogram, BucketNumbersMatchCs)
{
    EXPECT_EQ(LatencyHistogram::BucketOf(0), 0U);
    EXPECT_EQ(LatencyHistogram::BucketOf(15), 15U);
    EXPECT_EQ(LatencyHistogram::BucketOf(16), 16U);
    EXPECT_EQ(LatencyHistogram::BucketOf(32), 32U);
    EXPECT_EQ(LatencyHistogram::BucketOf(1'000'000), 270U);
}

TEST(LatencyHistogram, SmallValuesAreExact)
{
    LatencyHistogram histogram;
    for (auto value = int64_t{1}; value <= 10; ++value)
    {
        histogram.Record(std::chrono::nanoseconds{value});
    }
    const auto snapshot = histogram.Snapshot();

    EXPECT_EQ(snapshot.count, 10);
    EXPECT_EQ(snapshot.sum, 55);
    EXPECT_EQ(snapshot.p50, 5);
    EXPECT_EQ(snapshot.p90, 9);
    EXPECT_EQ(snapshot.max, 10);
}

// 틱 100번 중 98번은 1ms, 2번만 50ms 로 튀면 p50·p90 은 1ms 근처, p99 는 50ms 근처로 드러난다
TEST(LatencyHistogram, PercentilesShowRareSpikes)
{
    LatencyHistogram histogram;
    for (auto i = int32_t{0}; i < 98; ++i)
    {
        histogram.Record(1ms);
    }
    histogram.Record(50ms);
    histogram.Record(50ms);
    const auto snapshot = histogram.Snapshot();

    const auto near = [](const int64_t actual, const std::chrono::nanoseconds expected)
    { return actual >= expected.count() && actual <= expected.count() + expected.count() / 16; };
    EXPECT_TRUE(near(snapshot.p50, 1ms)) << snapshot.p50;
    EXPECT_TRUE(near(snapshot.p90, 1ms)) << snapshot.p90;
    EXPECT_TRUE(near(snapshot.p99, 50ms)) << snapshot.p99;
    EXPECT_EQ(snapshot.max, std::chrono::nanoseconds{50ms}.count());
    EXPECT_EQ(snapshot.p999, snapshot.max); // 칸 경계가 max 보다 크면 max 로 말한다
}

TEST(LatencyHistogram, NegativeAndHugeValues)
{
    LatencyHistogram histogram;
    histogram.Record(-5ns); // 0 으로 본다
    histogram.Record(100s); // 칸은 마지막, max 는 정확
    const auto snapshot = histogram.Snapshot();

    EXPECT_EQ(snapshot.count, 2);
    EXPECT_EQ(snapshot.max, std::chrono::nanoseconds{100s}.count());
    EXPECT_EQ(snapshot.p50, 0);
    EXPECT_EQ(snapshot.p99, snapshot.max);
}

TEST(LatencyHistogram, SnapshotAndResetStartsNewInterval)
{
    LatencyHistogram histogram;
    histogram.Record(2ms);
    histogram.Record(3ms);

    const auto first = histogram.SnapshotAndReset();
    const auto empty = histogram.SnapshotAndReset();
    histogram.Record(7ms);
    const auto third = histogram.SnapshotAndReset();

    EXPECT_EQ(first.count, 2);
    EXPECT_EQ(first.max, std::chrono::nanoseconds{3ms}.count());
    EXPECT_EQ(empty.count, 0);
    EXPECT_EQ(empty.max, 0);
    EXPECT_EQ(third.count, 1);
    EXPECT_EQ(third.max, std::chrono::nanoseconds{7ms}.count());
}

// 여러 스레드가 넣는 동안 다른 스레드가 구간마다 꺼내도 하나도 잃지 않는다
TEST(LatencyHistogram, KeepsEveryRecordAcrossConcurrentResets)
{
    constexpr auto Threads = int32_t{8};
    constexpr auto PerThread = int64_t{50'000};
    LatencyHistogram histogram;
    auto collected = int64_t{0};
    auto sum = int64_t{0};
    {
        std::vector<std::jthread> writers;
        writers.reserve(Threads);
        for (auto t = int32_t{0}; t < Threads; ++t)
        {
            writers.emplace_back(
                [&histogram, t]
                {
                    for (auto i = int64_t{0}; i < PerThread; ++i)
                    {
                        histogram.Record(std::chrono::nanoseconds{(i % 1000) + t});
                    }
                });
        }
        for (auto round = int32_t{0}; round < 20; ++round)
        {
            const auto part = histogram.SnapshotAndReset();
            collected += part.count;
            sum += part.sum;
        }
    } // 모두 join
    const auto rest = histogram.SnapshotAndReset();
    collected += rest.count;
    sum += rest.sum;

    EXPECT_EQ(collected, Threads * PerThread);
    auto expectedSum = int64_t{0};
    for (auto t = int32_t{0}; t < Threads; ++t)
    {
        for (auto i = int64_t{0}; i < PerThread; ++i)
        {
            expectedSum += (i % 1000) + t;
        }
    }
    EXPECT_EQ(sum, expectedSum);
}

TEST(ScopedTimer, RecordsElapsedTimeOfBlock)
{
    LatencyHistogram histogram;
    {
        Metrics::ScopedTimer timer{histogram};
        std::this_thread::sleep_for(5ms);
    }
    const auto snapshot = histogram.Snapshot();

    EXPECT_EQ(snapshot.count, 1);
    EXPECT_GE(snapshot.max, std::chrono::nanoseconds{5ms}.count());
    EXPECT_LT(snapshot.max, std::chrono::nanoseconds{2s}.count());
}
