#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "Metrics/MetricCounter.h"

using Metrics::MetricCounter;

TEST(MetricCounter, StartsAtZeroAndAdds)
{
    MetricCounter counter;
    EXPECT_EQ(counter.Value(), 0);

    counter.Add();
    counter.Add(41);

    EXPECT_EQ(counter.Value(), 42);
}

// 여러 스레드가 동시에 올려도 하나도 잃지 않는다. 스레드가 칸 수보다 많아 칸을 같이 쓰는 경우도 포함한다.
TEST(MetricCounter, CountsEveryAddFromManyThreads)
{
    constexpr auto Threads = int32_t{MetricCounter::StripeCount * 2 + 3};
    constexpr auto PerThread = int64_t{20'000};
    MetricCounter counter;
    MetricCounter bytes;
    {
        std::vector<std::jthread> workers;
        workers.reserve(Threads);
        for (auto t = int32_t{0}; t < Threads; ++t)
        {
            workers.emplace_back(
                [&]
                {
                    for (auto i = int64_t{0}; i < PerThread; ++i)
                    {
                        counter.Add();
                        bytes.Add(3);
                    }
                });
        }
    } // 모두 join: 그 뒤 Value()는 정확하다

    EXPECT_EQ(counter.Value(), Threads * PerThread);
    EXPECT_EQ(bytes.Value(), Threads * PerThread * 3);
}
