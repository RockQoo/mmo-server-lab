#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "Base/Stream.h"

namespace
{
    enum class Job : std::uint8_t
    {
        Warrior = 1,
        Archer = 2,
    };
} // namespace

// 쓴 순서대로 읽으면 같은 값이 나와야 한다.
TEST(Stream, PrimitivesRoundTrip)
{
    Base::Stream s;
    s << std::int8_t{-1} << std::uint16_t{65535} << std::int32_t{-123456} << std::uint64_t{1ull << 40};
    s << 3.5f << -0.25 << true << false << Job::Archer;

    auto i8 = std::int8_t{};
    auto u16 = std::uint16_t{};
    auto i32 = std::int32_t{};
    auto u64 = std::uint64_t{};
    auto f = float{};
    auto d = double{};
    auto t = false;
    auto f2 = true;
    auto job = Job::Warrior;
    s >> i8 >> u16 >> i32 >> u64 >> f >> d >> t >> f2 >> job;

    EXPECT_TRUE(s.IsValid());
    EXPECT_EQ(i8, -1);
    EXPECT_EQ(u16, 65535);
    EXPECT_EQ(i32, -123456);
    EXPECT_EQ(u64, 1ull << 40);
    EXPECT_EQ(f, 3.5f);
    EXPECT_EQ(d, -0.25);
    EXPECT_TRUE(t);
    EXPECT_FALSE(f2);
    EXPECT_EQ(job, Job::Archer);
    EXPECT_EQ(s.ReadPos(), s.Size());
}

// 형식이 C#과 같아야 하므로 바이트 배치를 직접 확인한다. 리틀 엔디안: 낮은 바이트가 먼저.
TEST(Stream, LittleEndianLayout)
{
    Base::Stream s;
    s << std::uint32_t{0x12345678} << true << Job::Archer;

    const auto data = s.Data();
    ASSERT_EQ(data.size(), 4u + 1u + 1u); // enum은 기반 타입(uint8_t) 크기
    EXPECT_EQ(data[0], std::byte{0x78});
    EXPECT_EQ(data[1], std::byte{0x56});
    EXPECT_EQ(data[2], std::byte{0x34});
    EXPECT_EQ(data[3], std::byte{0x12});
    EXPECT_EQ(data[4], std::byte{1});
    EXPECT_EQ(data[5], std::byte{2});
}

// 데이터가 모자라면 실패 표시가 남고, 값은 0이 되며, 이후 읽기도 무시된다.
TEST(Stream, ReadPastEndFails)
{
    Base::Stream s;
    s << std::uint16_t{7};

    auto tooBig = std::uint32_t{99};
    auto after = std::uint8_t{99};
    s >> tooBig >> after;

    EXPECT_FALSE(s.IsValid());
    EXPECT_EQ(tooBig, 0u);
    EXPECT_EQ(after, 0u);
    EXPECT_EQ(s.ReadPos(), 0u); // 실패한 읽기는 위치를 움직이지 않는다
}

// 받은 바이트로 만든 Stream에서 읽을 수 있어야 한다(네트워크에서 받은 경우).
TEST(Stream, ReadFromReceivedBytes)
{
    const std::byte received[] = {std::byte{0x2A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Base::Stream s(received);

    auto value = std::int32_t{};
    s >> value;

    EXPECT_TRUE(s.IsValid());
    EXPECT_EQ(value, 42);
}

// 0/1이 아닌 바이트가 와도 bool은 true/false 중 하나가 된다.
TEST(Stream, BoolFromAnyNonZeroByte)
{
    const std::byte received[] = {std::byte{0x7F}};
    Base::Stream s(received);

    auto value = false;
    s >> value;

    EXPECT_TRUE(value);
}

// Clear는 내용을 비우지만 확보한 메모리는 유지한다(재사용, 풀링 대비).
TEST(Stream, ClearKeepsCapacityForReuse)
{
    Base::Stream s;
    for (auto i = 0; i < 100; ++i)
    {
        s << std::numeric_limits<std::uint64_t>::max();
    }
    auto tooMuch = std::uint8_t{};
    Base::Stream empty;
    empty >> tooMuch; // 실패 상태 만들기
    ASSERT_FALSE(empty.IsValid());

    const auto capacityBefore = s.Capacity();
    ASSERT_GE(capacityBefore, 100u * sizeof(std::uint64_t));

    s.Clear();
    empty.Clear();

    EXPECT_EQ(s.Capacity(), capacityBefore);
    EXPECT_EQ(s.Size(), 0u);
    EXPECT_EQ(s.ReadPos(), 0u);
    EXPECT_TRUE(empty.IsValid());
}
