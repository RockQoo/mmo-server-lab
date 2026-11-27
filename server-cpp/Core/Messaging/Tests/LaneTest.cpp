#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include "Messaging/Lane.h"

using Messaging::Lane;

// Stop()이 스레드를 join하므로, Stop() 뒤에는 일이 쓴 값을 락 없이 읽어도 된다.

TEST(Lane, RunsJobsInPostedOrder)
{
    std::vector<int32_t> order;
    Lane lane;
    for (auto i = int32_t{0}; i < 1000; ++i)
    {
        ASSERT_TRUE(lane.Post([&order, i] { order.push_back(i); }));
    }
    lane.Stop();

    ASSERT_EQ(order.size(), 1000U);
    for (auto i = int32_t{0}; i < 1000; ++i)
    {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(Lane, RunsOnItsOwnThread)
{
    auto inLane = false;
    std::thread::id jobThread;
    Lane lane;
    ASSERT_TRUE(lane.Post(
        [&]
        {
            inLane = lane.IsInLane();
            jobThread = std::this_thread::get_id();
        }));
    lane.Stop();

    EXPECT_TRUE(inLane);
    EXPECT_FALSE(lane.IsInLane());
    EXPECT_NE(jobThread, std::this_thread::get_id());
}

// 다른 Lane의 일 안에서는 false. 멈춘 Lane의 스레드 ID를 새 Lane이 다시 받아도 마찬가지다.
TEST(Lane, IsInLaneIsFalseInOtherLane)
{
    Lane first;
    first.Stop();
    for (auto i = int32_t{0}; i < 20; ++i) // ID 재사용이 일어날 기회를 여러 번 준다
    {
        auto inFirst = true;
        Lane other;
        ASSERT_TRUE(other.Post([&] { inFirst = first.IsInLane(); }));
        other.Stop();
        EXPECT_FALSE(inFirst);
    }
}

// 여러 스레드가 동시에 넣어도 하나도 잃지 않고, 같은 생산자가 넣은 일끼리는 순서가 지켜진다.
TEST(Lane, KeepsOrderPerProducer)
{
    constexpr auto Producers = int32_t{4};
    constexpr auto PerProducer = int32_t{10000};
    std::vector<std::pair<int32_t, int32_t>> seen; // (생산자, 순번). Lane 스레드만 쓴다
    {
        Lane lane;
        std::vector<std::jthread> threads;
        for (auto p = int32_t{0}; p < Producers; ++p)
        {
            threads.emplace_back(
                [&lane, &seen, p]
                {
                    for (auto n = int32_t{0}; n < PerProducer; ++n)
                    {
                        // Post는 Stop 전이라 실패하지 않는다. 실패하면 아래 개수 확인에서 드러난다.
                        static_cast<void>(lane.Post([&seen, p, n] { seen.emplace_back(p, n); }));
                    }
                });
        }
        threads.clear(); // 생산자 join
    } // Lane 소멸 = Stop: 남은 일 처리 후 join

    ASSERT_EQ(seen.size(), static_cast<std::size_t>(Producers * PerProducer));
    std::vector<int32_t> next(Producers, 0);
    for (const auto& [p, n] : seen)
    {
        ASSERT_EQ(n, next[static_cast<std::size_t>(p)]) << "producer " << p;
        ++next[static_cast<std::size_t>(p)];
    }
}

// 멈출 때 이미 넣은 일은 버리지 않는다.
TEST(Lane, StopRunsRemainingJobs)
{
    std::atomic<int32_t> done{};
    {
        Lane lane;
        for (auto i = int32_t{0}; i < 100; ++i)
        {
            ASSERT_TRUE(lane.Post([&done] { ++done; }));
        }
    }
    EXPECT_EQ(done.load(), 100);
}

// 일 안에서 자기 Lane에 다음 일을 넣을 수 있다(락을 잡은 채 일을 돌리지 않으므로 막히지 않는다).
TEST(Lane, JobCanPostToItsOwnLane)
{
    std::promise<bool> second;
    Lane lane;
    ASSERT_TRUE(lane.Post(
        [&]
        {
            const auto posted = lane.Post([&second, &lane] { second.set_value(lane.IsInLane()); });
            EXPECT_TRUE(posted);
        }));

    EXPECT_TRUE(second.get_future().get());
}

TEST(Lane, RejectsAfterStop)
{
    Lane lane;
    lane.Stop();
    lane.Stop(); // 두 번 불러도 된다

    EXPECT_FALSE(lane.Post([] {}));
}

TEST(Lane, RejectsEmptyJob)
{
    Lane lane;
    EXPECT_FALSE(lane.Post(Lane::Job{}));
}

TEST(LaneDeathTest, StopFromOwnThreadCrashes)
{
    EXPECT_DEATH(
        {
            Lane lane;
            static_cast<void>(lane.Post([&lane] { lane.Stop(); }));
            lane.Stop();
        },
        "own thread");
}
