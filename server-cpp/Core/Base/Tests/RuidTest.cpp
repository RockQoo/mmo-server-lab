#include <gtest/gtest.h>

#include "Base/Ruid.h"

using Base::Ruid;

// 컴파일 시간에도 조립·분해가 된다(constexpr).
static_assert(Ruid::Make(5, 3, 2, 1).Realm() == 3);

TEST(Ruid, LayoutLimits)
{
    EXPECT_EQ(Ruid::MaxTimestamp, (int64_t{1} << 41) - 1);
    EXPECT_EQ(Ruid::MaxRealm, 127);
    EXPECT_EQ(Ruid::MaxNode, 63);
    EXPECT_EQ(Ruid::MaxSequence, 511);
}

// 각 칸의 최솟값·최댓값을 넣고 그대로 꺼내진다(칸끼리 비트가 겹치지 않는다).
TEST(Ruid, RoundTripsEachFieldAtLimits)
{
    const auto min = Ruid::Make(0, Ruid::MinRealm, 0, 0);
    EXPECT_EQ(min.Timestamp(), 0);
    EXPECT_EQ(min.Realm(), Ruid::MinRealm);
    EXPECT_EQ(min.Node(), 0);
    EXPECT_EQ(min.Sequence(), 0);

    const auto max = Ruid::Make(Ruid::MaxTimestamp, Ruid::MaxRealm, Ruid::MaxNode, Ruid::MaxSequence);
    EXPECT_EQ(max.Timestamp(), Ruid::MaxTimestamp);
    EXPECT_EQ(max.Realm(), Ruid::MaxRealm);
    EXPECT_EQ(max.Node(), Ruid::MaxNode);
    EXPECT_EQ(max.Sequence(), Ruid::MaxSequence);

    const auto mixed = Ruid::Make(123'456'789, 42, 17, 300);
    EXPECT_EQ(mixed.Timestamp(), 123'456'789);
    EXPECT_EQ(mixed.Realm(), 42);
    EXPECT_EQ(mixed.Node(), 17);
    EXPECT_EQ(mixed.Sequence(), 300);
}

// 시각이 맨 위라 1ms라도 늦게 만든 ID가 항상 크다(다른 칸이 모두 최댓값이어도).
TEST(Ruid, LaterTimestampIsAlwaysGreater)
{
    const auto earlier = Ruid::Make(1000, Ruid::MaxRealm, Ruid::MaxNode, Ruid::MaxSequence);
    const auto later = Ruid::Make(1001, Ruid::MinRealm, 0, 0);
    EXPECT_LT(earlier.Value(), later.Value());
    EXPECT_LT(earlier, later);
}

// 부호 비트는 늘 0이라 가장 큰 ID도 양수다.
TEST(Ruid, AlwaysPositive)
{
    const auto max = Ruid::Make(Ruid::MaxTimestamp, Ruid::MaxRealm, Ruid::MaxNode, Ruid::MaxSequence);
    EXPECT_GT(max.Value(), 0);
}

// Realm 0이 예약이라 가장 작은 ID도 0(Ruid{})이 아니다.
TEST(Ruid, NoneIsNeverIssued)
{
    EXPECT_EQ(Ruid{}.Value(), 0);
    EXPECT_NE(Ruid::Make(0, Ruid::MinRealm, 0, 0), Ruid{});
}

TEST(Ruid, UnixMsAddsEpoch)
{
    EXPECT_EQ(Ruid::Make(0, 1, 0, 0).UnixMs(), Ruid::EpochUnixMs);
    EXPECT_EQ(Ruid::Make(1500, 1, 0, 0).UnixMs(), Ruid::EpochUnixMs + 1500);
}

// 범위를 벗어나면 잘못된 ID를 만들지 않고 멈춘다.
TEST(RuidDeathTest, RejectsOutOfRangeFields)
{
    EXPECT_DEATH(static_cast<void>(Ruid::Make(-1, 1, 0, 0)), "timestamp");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(Ruid::MaxTimestamp + 1, 1, 0, 0)), "timestamp");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, 0, 0, 0)), "realm");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, Ruid::MaxRealm + 1, 0, 0)), "realm");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, 1, -1, 0)), "node");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, 1, Ruid::MaxNode + 1, 0)), "node");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, 1, 0, -1)), "sequence");
    EXPECT_DEATH(static_cast<void>(Ruid::Make(0, 1, 0, Ruid::MaxSequence + 1)), "sequence");
}
