#include <gtest/gtest.h>

#include "Base/Ruid.h"

using Base::Ruid;

// 컴파일 시간에도 조립·분해가 된다(constexpr).
static_assert(Base::RuidRealm(Base::MakeRuid(5, 3, 2, 1)) == 3);

TEST(Ruid, LayoutLimits)
{
    EXPECT_EQ(Base::RuidMaxTimestamp, (std::int64_t{1} << 41) - 1);
    EXPECT_EQ(Base::RuidMaxRealm, 127);
    EXPECT_EQ(Base::RuidMaxNode, 63);
    EXPECT_EQ(Base::RuidMaxSequence, 511);
}

// 각 칸의 최솟값·최댓값을 넣고 그대로 꺼내진다(칸끼리 비트가 겹치지 않는다).
TEST(Ruid, RoundTripsEachFieldAtLimits)
{
    const auto min = Base::MakeRuid(0, Base::RuidMinRealm, 0, 0);
    EXPECT_EQ(Base::RuidTimestamp(min), 0);
    EXPECT_EQ(Base::RuidRealm(min), Base::RuidMinRealm);
    EXPECT_EQ(Base::RuidNode(min), 0);
    EXPECT_EQ(Base::RuidSequence(min), 0);

    const auto max =
        Base::MakeRuid(Base::RuidMaxTimestamp, Base::RuidMaxRealm, Base::RuidMaxNode, Base::RuidMaxSequence);
    EXPECT_EQ(Base::RuidTimestamp(max), Base::RuidMaxTimestamp);
    EXPECT_EQ(Base::RuidRealm(max), Base::RuidMaxRealm);
    EXPECT_EQ(Base::RuidNode(max), Base::RuidMaxNode);
    EXPECT_EQ(Base::RuidSequence(max), Base::RuidMaxSequence);

    const auto mixed = Base::MakeRuid(123'456'789, 42, 17, 300);
    EXPECT_EQ(Base::RuidTimestamp(mixed), 123'456'789);
    EXPECT_EQ(Base::RuidRealm(mixed), 42);
    EXPECT_EQ(Base::RuidNode(mixed), 17);
    EXPECT_EQ(Base::RuidSequence(mixed), 300);
}

// 시각이 맨 위라 1ms라도 늦게 만든 ID가 항상 크다(다른 칸이 모두 최댓값이어도).
TEST(Ruid, LaterTimestampIsAlwaysGreater)
{
    const auto earlier = Base::MakeRuid(1000, Base::RuidMaxRealm, Base::RuidMaxNode, Base::RuidMaxSequence);
    const auto later = Base::MakeRuid(1001, Base::RuidMinRealm, 0, 0);
    EXPECT_LT(earlier.Value(), later.Value());
    EXPECT_LT(earlier, later);
}

// 부호 비트는 늘 0이라 가장 큰 ID도 양수다.
TEST(Ruid, AlwaysPositive)
{
    const auto max =
        Base::MakeRuid(Base::RuidMaxTimestamp, Base::RuidMaxRealm, Base::RuidMaxNode, Base::RuidMaxSequence);
    EXPECT_GT(max.Value(), 0);
}

// Realm 0이 예약이라 가장 작은 ID도 0(Ruid{})이 아니다.
TEST(Ruid, NoneIsNeverIssued)
{
    EXPECT_EQ(Ruid{}.Value(), 0);
    EXPECT_NE(Base::MakeRuid(0, Base::RuidMinRealm, 0, 0), Ruid{});
}

TEST(Ruid, UnixMsAddsEpoch)
{
    EXPECT_EQ(Base::RuidUnixMs(Base::MakeRuid(0, 1, 0, 0)), Base::RuidEpochUnixMs);
    EXPECT_EQ(Base::RuidUnixMs(Base::MakeRuid(1500, 1, 0, 0)), Base::RuidEpochUnixMs + 1500);
}

// 범위를 벗어나면 잘못된 ID를 만들지 않고 멈춘다.
TEST(RuidDeathTest, RejectsOutOfRangeFields)
{
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(-1, 1, 0, 0)), "timestamp");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(Base::RuidMaxTimestamp + 1, 1, 0, 0)), "timestamp");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, 0, 0, 0)), "realm");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, Base::RuidMaxRealm + 1, 0, 0)), "realm");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, 1, -1, 0)), "node");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, 1, Base::RuidMaxNode + 1, 0)), "node");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, 1, 0, -1)), "sequence");
    EXPECT_DEATH(static_cast<void>(Base::MakeRuid(0, 1, 0, Base::RuidMaxSequence + 1)), "sequence");
}
