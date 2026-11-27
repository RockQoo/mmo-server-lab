#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <thread>
#include <vector>

#include "Messaging/Lane.h"
#include "Scheduling/Scheduler.h"

// 실제 시계로 돈다. 시각은 짧게(ms) 잡고, 기다릴 때는 시간 제한을 둔다(느린 PC 에서도 넉넉하게 2초).
// Lane 을 Scheduler 보다 먼저 선언한다: 거꾸로 사라지므로 Scheduler 가 먼저 멈춘다(Scheduler.h 규칙).
using namespace std::chrono_literals;
using Messaging::Lane;
using Scheduling::Scheduler;
using Scheduling::TimerId;

namespace
{
    constexpr auto Patience = 2s;

    bool WaitUntil(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + Patience;
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    // 지금까지 lane 에 넣은 일이 모두 끝날 때까지 기다린다
    void Drain(Lane& lane)
    {
        std::promise<void> done;
        ASSERT_TRUE(lane.Post([&] { done.set_value(); }));
        ASSERT_EQ(done.get_future().wait_for(Patience), std::future_status::ready);
    }
} // namespace

TEST(Scheduler, AfterRunsOnceOnGivenLane)
{
    Lane lane;
    Scheduler scheduler;
    std::promise<bool> ranInLane;
    std::atomic<int32_t> count{};

    scheduler.After(10ms, lane,
                    [&]
                    {
                        if (++count == 1)
                        {
                            ranInLane.set_value(lane.IsInLane());
                        }
                    });

    auto future = ranInLane.get_future();
    ASSERT_EQ(future.wait_for(Patience), std::future_status::ready);
    EXPECT_TRUE(future.get()); // 타이머 스레드가 아니라 Lane 에서
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count, 1);
}

TEST(Scheduler, EveryRepeatsUntilCanceled)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<int32_t> count{};

    const auto id = scheduler.Every(5ms, lane, [&] { ++count; });
    ASSERT_TRUE(WaitUntil([&] { return count >= 3; }));
    EXPECT_TRUE(scheduler.Cancel(id));
    EXPECT_FALSE(scheduler.Cancel(id));
    Drain(lane);
    const auto afterCancel = count.load();

    std::this_thread::sleep_for(50ms);
    Drain(lane);
    EXPECT_EQ(count, afterCancel);
}

TEST(Scheduler, CanceledBeforeDueDoesNotRun)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<bool> ran{};

    const auto id = scheduler.After(30ms, lane, [&] { ran = true; });
    EXPECT_TRUE(scheduler.Cancel(id));
    std::this_thread::sleep_for(80ms);
    Drain(lane);

    EXPECT_FALSE(ran);
    EXPECT_FALSE(scheduler.Cancel(TimerId{}));
}

// 시각이 돼 Lane 에 이미 넣었지만 Lane 이 바빠 아직 안 돈 회차도 Cancel 이 막는다(버프 만료 직전 해제 같은 경우)
TEST(Scheduler, CancelStopsRunAlreadyPostedToBusyLane)
{
    Lane lane;
    Scheduler scheduler;
    std::promise<void> release;
    auto gate = release.get_future().share();
    ASSERT_TRUE(lane.Post([gate] { gate.wait(); })); // Lane 을 막아 둔다
    std::atomic<bool> ran{};

    const auto id = scheduler.After(1ms, lane, [&] { ran = true; });
    std::this_thread::sleep_for(50ms); // 타이머 스레드가 회차를 넣을 시간
    EXPECT_TRUE(scheduler.Cancel(id));
    release.set_value();
    Drain(lane);

    EXPECT_FALSE(ran);
}

TEST(Scheduler, OneShotCancelAfterRunReturnsFalse)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<bool> ran{};

    const auto id = scheduler.After(1ms, lane, [&] { ran = true; });
    ASSERT_TRUE(WaitUntil([&] { return ran.load(); }));
    Drain(lane);

    EXPECT_FALSE(scheduler.Cancel(id));
}

// 먼 일을 기다리며 자는 중에 더 이른 일이 들어오면 깨서 그것부터 돌린다
TEST(Scheduler, EarlierTimerWakesSleepingThread)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<bool> early{};
    std::atomic<bool> late{};

    scheduler.After(1h, lane, [&] { late = true; });
    std::this_thread::sleep_for(10ms); // 타이머 스레드가 1시간 잠에 들어간 뒤
    scheduler.After(5ms, lane, [&] { early = true; });

    EXPECT_TRUE(WaitUntil([&] { return early.load(); }));
    EXPECT_FALSE(late);
}

// 주기 일이 자기 Lane 안에서 자기 자신을 지우면 그 뒤로 돌지 않는다(독 데미지가 HP 0 에서 멈춤)
TEST(Scheduler, PeriodicJobCanCancelItselfInLane)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<int32_t> count{};
    std::atomic<uint64_t> idValue{};

    const auto id = scheduler.Every(2ms, lane,
                                    [&]
                                    {
                                        if (++count == 3)
                                        {
                                            EXPECT_TRUE(scheduler.Cancel(TimerId{idValue.load()}));
                                        }
                                    });
    idValue = id.Value();
    ASSERT_TRUE(WaitUntil([&] { return count >= 3; }));
    std::this_thread::sleep_for(30ms);
    Drain(lane);

    EXPECT_EQ(count, 3);
}

TEST(Scheduler, StopDropsPendingAndRejectsNew)
{
    Lane lane;
    Scheduler scheduler;
    std::atomic<bool> ran{};

    scheduler.After(30ms, lane, [&] { ran = true; });
    scheduler.Stop();
    scheduler.Stop(); // 두 번 불러도 된다

    EXPECT_EQ(scheduler.After(1ms, lane, [&] { ran = true; }), TimerId{});
    std::this_thread::sleep_for(60ms);
    Drain(lane);
    EXPECT_FALSE(ran);
}

// 여러 스레드가 같이 Stop 해도 join 은 한 번만 하고 모두 돌아온다(리뷰 R1). 경합이 나도록 같이 출발시켜 여러 번 한다.
TEST(Scheduler, ConcurrentStopIsSafe)
{
    constexpr int32_t Rounds = 50;
    constexpr int32_t Stoppers = 4;
    for (int32_t round = 0; round < Rounds; ++round)
    {
        Lane lane;
        Scheduler scheduler;
        scheduler.Every(1ms, lane, [] {});
        std::latch start{Stoppers};
        std::vector<std::jthread> stoppers;
        stoppers.reserve(Stoppers);
        for (int32_t i = 0; i < Stoppers; ++i)
        {
            stoppers.emplace_back(
                [&]
                {
                    start.arrive_and_wait();
                    scheduler.Stop();
                });
        }
        stoppers.clear(); // 모두 join
        EXPECT_EQ(scheduler.After(1ms, lane, [] {}), TimerId{});
    }
}
