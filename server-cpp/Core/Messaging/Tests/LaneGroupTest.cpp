#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Base/Ruid.h"
#include "Messaging/LaneGroup.h"

using Messaging::LaneGroup;

// LaneGroup 은 () 로 만든다. {} 안에 문자열(→ string_view 변환)이 있으면 MSVC 평가 순서 경고(C4868)가 난다.

TEST(LaneGroup, SameKeyAlwaysSameLane)
{
    LaneGroup group("Test", 8);
    for (auto key = uint64_t{0}; key < 1000; ++key)
    {
        ASSERT_EQ(&group.LaneFor(key), &group.LaneFor(key));
    }
}

// StrongId 계열(Ruid)은 정수 값으로 고른다.
TEST(LaneGroup, StrongIdKeyUsesValue)
{
    LaneGroup group("Test", 8);
    const auto id = Base::Ruid::Make(123'456, 1, 3, 0);
    EXPECT_EQ(&group.LaneFor(id), &group.LaneFor(static_cast<uint64_t>(id.Value())));
}

// 섞기 결과가 공식(splitmix64 마무리)과 같다. C# 테스트도 같은 값을 확인한다(두 서버가 같은 Lane을 고른다).
TEST(LaneGroup, IndexForMatchesKnownValues)
{
    EXPECT_EQ(LaneGroup::IndexFor(0, 8), 0u);
    EXPECT_EQ(LaneGroup::IndexFor(1, 8), 5u); // Mix(1) = 0x5692161D100B05E5
    EXPECT_EQ(LaneGroup::IndexFor(2, 8), 2u); // Mix(2) = 0xDBD238973A2B148A
    EXPECT_EQ(LaneGroup::IndexFor(3, 8), 0u); // Mix(3) = 0x1E535EEDE31428F0
    EXPECT_EQ(LaneGroup::IndexFor(12345, 7), 6u);
}

// 드물게 만든 Ruid(순번이 늘 0)도 고르게 퍼진다. 같은 ID를 key % 8로 나누면 전부 Lane 0이다(섞는 이유).
TEST(LaneGroup, SpreadsSparseRuids)
{
    constexpr auto LaneCount = std::size_t{8};
    constexpr auto IdCount = 1000;
    std::array<int, LaneCount> mixed{};
    std::array<int, LaneCount> plainModulo{};
    for (auto i = 0; i < IdCount; ++i)
    {
        const auto id = Base::Ruid::Make(1000 + int64_t{i} * 7919, 1, 3, 0); // 서로 다른 ms, 순번 0
        const auto key = static_cast<uint64_t>(id.Value());
        ++mixed[LaneGroup::IndexFor(key, LaneCount)];
        ++plainModulo[key % LaneCount];
    }

    EXPECT_EQ(plainModulo[0], IdCount) << "key % 8 은 순번 비트만 봐서 전부 Lane 0";
    constexpr auto Mean = IdCount / static_cast<int>(LaneCount);
    for (const auto count : mixed)
    {
        EXPECT_GT(count, Mean * 6 / 10);
        EXPECT_LT(count, Mean * 14 / 10);
    }
}

// 번호로 고른 Lane은 서로 다르고, 스레드 이름이 "이름-번호"다(VS 스레드 창·덤프에 보이는 이름).
TEST(LaneGroup, AtGivesNamedDistinctLanes)
{
    LaneGroup group("Zone", 4);
    ASSERT_EQ(group.Count(), 4u);
    EXPECT_NE(&group.At(0), &group.At(1));

    std::promise<std::wstring> name;
    ASSERT_TRUE(group.At(2).Post(
        [&name]
        {
            PWSTR description{};
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)))
            {
                name.set_value(description);
                LocalFree(description);
            }
            else
            {
                name.set_value(L"");
            }
        }));
    EXPECT_EQ(name.get_future().get(), L"Zone-2");
}

// 같은 키의 일은 넣은 순서대로 처리된다. 생산자 4개가 각자 자기 키로 2000개씩 동시에 넣는다.
// 키마다 기록하는 vector는 그 키의 Lane만 만지므로 락이 없다.
TEST(LaneGroup, KeepsOrderPerKey)
{
    constexpr auto ProducerCount = 4;
    constexpr auto PerProducer = 2000;
    std::array<std::vector<int>, ProducerCount> seen{};
    {
        LaneGroup group("Test", 3); // Lane보다 키가 많아 한 Lane에 여러 키가 섞이게 한다
        std::vector<std::jthread> producers;
        for (auto p = 0; p < ProducerCount; ++p)
        {
            producers.emplace_back(
                [&group, &seen, p]
                {
                    auto& lane = group.LaneFor(static_cast<uint64_t>(p));
                    auto& mine = seen[static_cast<std::size_t>(p)];
                    for (auto i = 0; i < PerProducer; ++i)
                    {
                        ASSERT_TRUE(lane.Post([&mine, i] { mine.push_back(i); }));
                    }
                });
        }
    } // 생산자 join → 그다음 group 소멸(모든 Lane이 남은 일을 처리하고 멈춤)

    for (const auto& mine : seen)
    {
        ASSERT_EQ(mine.size(), static_cast<std::size_t>(PerProducer));
        for (auto i = 0; i < PerProducer; ++i)
        {
            ASSERT_EQ(mine[static_cast<std::size_t>(i)], i);
        }
    }
}

TEST(LaneGroupDeathTest, RejectsZeroLanes)
{
    EXPECT_DEATH(LaneGroup("Test", 0), "at least one Lane");
}

TEST(LaneGroupDeathTest, AtOutOfRangeCrashes)
{
    EXPECT_DEATH(
        {
            LaneGroup group("Test", 2); // 매크로 안이라 {} 의 쉼표가 인자 구분으로 읽히지 않게 () 로 만든다
            static_cast<void>(group.At(2));
        },
        "out of range");
}
