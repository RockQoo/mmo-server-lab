#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "Scheduling/TimerHeap.h"

// 시계 없이 시각을 직접 넣는다. Start 는 아무 시각이나 되고, 모든 테스트가 실제로 기다리지 않는다.
using namespace std::chrono_literals;
using Scheduling::TimerHeap;
using Scheduling::TimerId;

namespace
{
    const auto Start = TimerHeap::Clock::now();
}

TEST(TimerHeap, RunsOnlyDueJobsInTimeOrder)
{
    TimerHeap heap;
    std::vector<std::string> ran;
    heap.Add(Start + 3s, [&] { ran.emplace_back("3s"); });
    heap.Add(Start + 1s, [&] { ran.emplace_back("1s"); });
    heap.Add(Start + 2s, [&] { ran.emplace_back("2s"); });

    EXPECT_EQ(heap.RunDue(Start + 500ms), 0U);
    EXPECT_EQ(heap.RunDue(Start + 2s), 2U); // 시각이 딱 된 것도 실행
    EXPECT_EQ(ran, (std::vector<std::string>{"1s", "2s"}));
    EXPECT_EQ(heap.Size(), 1U);
    EXPECT_EQ(heap.NextDue(), Start + 3s);
}

TEST(TimerHeap, SameTimeRunsInAddOrder)
{
    TimerHeap heap;
    std::vector<int32_t> ran;
    for (auto i = int32_t{0}; i < 5; ++i)
    {
        heap.Add(Start + 1s, [&ran, i] { ran.push_back(i); });
    }

    heap.RunDue(Start + 1s);

    EXPECT_EQ(ran, (std::vector<int32_t>{0, 1, 2, 3, 4}));
}

// 고정 간격: 늦게 실행해도 다음 예정은 예정 시각 + 주기(밀리지 않는다)
TEST(TimerHeap, RepeatsAtFixedRate)
{
    TimerHeap heap;
    auto count = int32_t{0};
    heap.Add(Start + 5s, [&] { ++count; }, 5s);

    heap.RunDue(Start + 6s); // 1초 늦게
    EXPECT_EQ(count, 1);
    EXPECT_EQ(heap.NextDue(), Start + 10s); // 11s 가 아니다

    heap.RunDue(Start + 10s);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(heap.NextDue(), Start + 15s);
    EXPECT_EQ(heap.Size(), 1U);
}

// 주기를 여러 번 놓쳤으면 몰아서 실행하지 않고 한 번만, 다음은 지금 다음 칸
TEST(TimerHeap, RunsMissedPeriodsOnlyOnce)
{
    TimerHeap heap;
    auto count = int32_t{0};
    heap.Add(Start + 1s, [&] { ++count; }, 1s);

    heap.RunDue(Start + 10500ms); // 1~10초, 10칸을 놓침

    EXPECT_EQ(count, 1);
    EXPECT_EQ(heap.NextDue(), Start + 11s);
}

TEST(TimerHeap, CanceledJobDoesNotRun)
{
    TimerHeap heap;
    auto ran = false;
    const auto id = heap.Add(Start + 1s, [&] { ran = true; });
    heap.Add(Start + 2s, [] {});

    EXPECT_TRUE(heap.Cancel(id));
    EXPECT_FALSE(heap.Cancel(id));         // 두 번째는 없음
    EXPECT_FALSE(heap.Cancel(TimerId{}));  // 0 = 없음
    EXPECT_EQ(heap.NextDue(), Start + 2s); // 지운 칸은 건너뛴다
    heap.RunDue(Start + 5s);

    EXPECT_FALSE(ran);
    EXPECT_EQ(heap.Size(), 0U);
    EXPECT_EQ(heap.NextDue(), std::nullopt);
}

// 주기 일이 자기 자신을 지우면(예: 독 데미지가 HP 0 에서 멈춤) 다시 들어가지 않는다
TEST(TimerHeap, PeriodicJobCanCancelItself)
{
    TimerHeap heap;
    auto count = int32_t{0};
    TimerId id;
    id = heap.Add(
        Start + 1s,
        [&]
        {
            if (++count == 3)
            {
                EXPECT_TRUE(heap.Cancel(id));
            }
        },
        1s);

    for (auto second = int32_t{1}; second <= 10; ++second)
    {
        heap.RunDue(Start + std::chrono::seconds{second});
    }

    EXPECT_EQ(count, 3);
    EXPECT_EQ(heap.Size(), 0U);
}

// 한 번짜리 일은 실행 중에 자기 자신을 지워도 이미 실행된 것이라 false
TEST(TimerHeap, OneShotJobCancelingItselfReturnsFalse)
{
    TimerHeap heap;
    TimerId id;
    auto canceled = true;
    id = heap.Add(Start + 1s, [&] { canceled = heap.Cancel(id); });

    heap.RunDue(Start + 1s);

    EXPECT_FALSE(canceled);
}

// 일 안에서 넣은 일도 시각이 됐으면 같은 RunDue 에서 실행된다(리스폰 → 바로 버프 같은 연쇄)
TEST(TimerHeap, JobAddedInsideJobRunsIfDue)
{
    TimerHeap heap;
    std::vector<std::string> ran;
    heap.Add(Start + 1s,
             [&]
             {
                 ran.emplace_back("first");
                 heap.Add(Start + 1s, [&] { ran.emplace_back("chained"); });
                 heap.Add(Start + 9s, [&] { ran.emplace_back("later"); });
             });

    heap.RunDue(Start + 2s);

    EXPECT_EQ(ran, (std::vector<std::string>{"first", "chained"}));
    EXPECT_EQ(heap.NextDue(), Start + 9s);
}

// 오래 뒤 일을 많이 지워도 힙이 지운 칸으로 계속 커지지 않는다(다시 만들어도 순서는 그대로)
TEST(TimerHeap, StaysCorrectAfterManyCancels)
{
    TimerHeap heap;
    std::vector<TimerId> ids;
    for (auto i = int32_t{0}; i < 1000; ++i)
    {
        ids.push_back(heap.Add(Start + std::chrono::minutes{30 + i}, [] {}));
    }
    std::vector<int32_t> ran;
    heap.Add(Start + 1s, [&] { ran.push_back(1); });
    heap.Add(Start + 2s, [&] { ran.push_back(2); });

    for (const auto id : ids)
    {
        EXPECT_TRUE(heap.Cancel(id));
    }

    EXPECT_EQ(heap.Size(), 2U);
    EXPECT_EQ(heap.NextDue(), Start + 1s);
    heap.RunDue(Start + 1h);
    EXPECT_EQ(ran, (std::vector<int32_t>{1, 2}));
}
