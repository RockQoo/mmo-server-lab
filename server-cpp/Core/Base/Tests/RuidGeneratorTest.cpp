#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Base/RuidGenerator.h"

using Base::Ruid;
using Base::RuidGenerator;

// 발급기는 () 로 만든다. 인자에 함수 호출(clock.Get())이 있으면 {} 초기화는 MSVC 평가 순서 경고(C4868)가 난다.
namespace
{
    // 테스트가 직접 움직이는 가짜 시계(기준 시각부터 지난 ms)
    struct FakeClock
    {
        std::shared_ptr<std::atomic<std::int64_t>> now{std::make_shared<std::atomic<std::int64_t>>(1000)};

        RuidGenerator::Clock Get() const
        {
            return [now = now] { return now->load(); };
        }
        void Set(const std::int64_t ms) const
        {
            now->store(ms);
        }
    };
} // namespace

TEST(RuidGenerator, IdCarriesRealmNodeAndTime)
{
    const FakeClock clock;
    RuidGenerator generator(3, 7, clock.Get());

    const auto id = generator.Next();
    EXPECT_EQ(id.Timestamp(), 1000);
    EXPECT_EQ(id.Realm(), 3);
    EXPECT_EQ(id.Node(), 7);
    EXPECT_EQ(id.Sequence(), 0);
}

// 같은 ms 안에서는 순번이 오르고, ms가 바뀌면 0부터 다시 센다.
TEST(RuidGenerator, SequenceCountsWithinSameMs)
{
    const FakeClock clock;
    RuidGenerator generator(1, 0, clock.Get());

    EXPECT_EQ(generator.Next().Sequence(), 0);
    EXPECT_EQ(generator.Next().Sequence(), 1);
    EXPECT_EQ(generator.Next().Sequence(), 2);

    clock.Set(1001);
    const auto id = generator.Next();
    EXPECT_EQ(id.Timestamp(), 1001);
    EXPECT_EQ(id.Sequence(), 0);
}

// 같은 ms에 512개를 다 쓰면 기다렸다가 다음 ms의 순번 0을 낸다(겹치거나 넘치지 않는다).
TEST(RuidGenerator, WaitsForNextMsAfter512)
{
    // 시계를 513번째로 볼 때부터 1001ms가 된다(512번은 1000ms, 513번째 호출은 한 번 막힌 뒤 1001ms).
    auto calls = std::make_shared<std::atomic<int>>(0);
    RuidGenerator generator(1, 0, [calls] { return calls->fetch_add(1) < 513 ? 1000 : 1001; });

    for (auto i = 0; i <= Ruid::MaxSequence; ++i)
    {
        const auto id = generator.Next();
        ASSERT_EQ(id.Timestamp(), 1000);
        ASSERT_EQ(id.Sequence(), i);
    }
    const auto next = generator.Next();
    EXPECT_EQ(next.Timestamp(), 1001);
    EXPECT_EQ(next.Sequence(), 0);
    EXPECT_EQ(generator.SequenceWaitCount(), 1); // 기다린 호출은 513번째 하나뿐
}

// 시계가 뒤로 가도(가짜 시계로 흉내) 마지막 시각을 유지해 겹치지 않고 계속 커진다.
TEST(RuidGenerator, ClockGoingBackStillIncreases)
{
    const FakeClock clock;
    RuidGenerator generator(1, 0, clock.Get());

    const auto before = generator.Next();
    clock.Set(990);
    const auto after = generator.Next();
    EXPECT_LT(before, after);
    EXPECT_EQ(after.Timestamp(), 1000);
    EXPECT_EQ(after.Sequence(), 1);
}

// 실제 시계: 한 스레드에서 연속으로 만들면 늘 앞의 것보다 크다.
TEST(RuidGenerator, RealClockIdsAlwaysIncrease)
{
    RuidGenerator generator(1, 0);
    auto previous = generator.Next();
    for (auto i = 0; i < 20'000; ++i)
    {
        const auto id = generator.Next();
        ASSERT_LT(previous, id);
        previous = id;
    }
}

// 핵심 보장: 스레드 200개가 동시에 만들어도 같은 ID가 하나도 없다.
// 스레드마다 만든 순서대로 커지는지도 본다. 실제 시계를 쓰고, latch로 한꺼번에 출발시켜 경쟁을 최대로 만든다.
TEST(RuidGenerator, ManyThreadsNeverDuplicate)
{
    constexpr auto ThreadCount = 200;
    constexpr auto PerThread = 2'500; // 총 50만 개(ms당 512개 상한이라 1초 남짓 걸린다)

    RuidGenerator generator(5, 9);
    std::vector<std::vector<Ruid>> results(ThreadCount);
    std::latch start{ThreadCount};
    {
        std::vector<std::jthread> threads;
        threads.reserve(ThreadCount);
        for (auto t = 0; t < ThreadCount; ++t)
        {
            threads.emplace_back(
                [&, t]
                {
                    auto& mine = results[static_cast<std::size_t>(t)];
                    mine.reserve(PerThread);
                    start.arrive_and_wait();
                    for (auto i = 0; i < PerThread; ++i)
                    {
                        mine.push_back(generator.Next());
                    }
                });
        }
    } // jthread는 소멸할 때 join한다

    std::vector<Ruid> all;
    all.reserve(static_cast<std::size_t>(ThreadCount) * PerThread);
    for (const auto& mine : results)
    {
        ASSERT_EQ(mine.size(), static_cast<std::size_t>(PerThread));
        // 앞의 것보다 크거나 같은(>=) 이웃이 없으면 = 늘 커졌다
        EXPECT_EQ(std::ranges::adjacent_find(mine, std::greater_equal<>{}), mine.end())
            << "한 스레드 안에서 커지지 않은 ID가 있다";
        all.insert(all.end(), mine.begin(), mine.end());
    }

    std::ranges::sort(all);
    const auto duplicate = std::ranges::adjacent_find(all);
    EXPECT_EQ(duplicate, all.end()) << "중복 ID: " << duplicate->Value();

    for (const auto id : all)
    {
        ASSERT_EQ(id.Realm(), 5);
        ASSERT_EQ(id.Node(), 9);
    }
}

// 노드 번호가 다르면 같은 ms, 같은 순번이어도 겹치지 않는다.
TEST(RuidGenerator, DifferentNodesNeverCollide)
{
    const FakeClock clock;
    RuidGenerator nodeA(1, 1, clock.Get());
    RuidGenerator nodeB(1, 2, clock.Get());

    std::unordered_set<Ruid> seen;
    for (auto i = 0; i < 300; ++i)
    {
        EXPECT_TRUE(seen.insert(nodeA.Next()).second);
        EXPECT_TRUE(seen.insert(nodeB.Next()).second);
    }
}

// 주의사항 1을 보여 준다: 노드 번호가 같은 두 프로세스(여기선 두 발급기)는 같은 ID를 낸다.
TEST(RuidGenerator, SameNodeCollidesWhichIsWhyNodesMustDiffer)
{
    const FakeClock clock;
    RuidGenerator processA(1, 1, clock.Get());
    RuidGenerator processB(1, 1, clock.Get());
    EXPECT_EQ(processA.Next(), processB.Next());
}

// 보통 쓰는 입구: Ruid::Init / Ruid::Create (안에서 전역 발급기를 쓴다).
TEST(RuidGenerator, RuidInitThenCreate)
{
    Ruid::Init(2, 4);
    const auto id = Ruid::Create();
    EXPECT_EQ(id.Realm(), 2);
    EXPECT_EQ(id.Node(), 4);
}

// 죽는 테스트는 자식 프로세스에서 돈다. 그래서 전역 발급기도 매번 새로 시작한다.
TEST(RuidGeneratorDeathTest, RejectsBadConfiguration)
{
    EXPECT_DEATH(RuidGenerator(0, 0), "realm");
    EXPECT_DEATH(RuidGenerator(Ruid::MaxRealm + 1, 0), "realm");
    EXPECT_DEATH(RuidGenerator(1, -1), "node");
    EXPECT_DEATH(RuidGenerator(1, Ruid::MaxNode + 1), "node");
}

TEST(RuidGeneratorDeathTest, RejectsClockOutOfRange)
{
    RuidGenerator generator(1, 0, [] { return std::int64_t{-1}; });
    EXPECT_DEATH(static_cast<void>(generator.Next()), "clock out of range");
}

TEST(RuidGeneratorDeathTest, CreateBeforeInitCrashes)
{
    EXPECT_DEATH(static_cast<void>(Ruid::Create()), "not called");
}

TEST(RuidGeneratorDeathTest, InitTwiceCrashes)
{
    EXPECT_DEATH(
        {
            Ruid::Init(1, 0);
            Ruid::Init(1, 1);
        },
        "called twice");
}
