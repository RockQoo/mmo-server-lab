#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Base/BinaryReader.h"
#include "Base/BinaryWriter.h"

namespace
{
    enum class Job : std::uint8_t
    {
        Warrior = 1,
        Archer = 2,
    };

    struct Item
    {
        std::int32_t id{};
        std::string name{};

        SERIALIZE_FIELDS(id, name)
        bool operator==(const Item&) const = default;
    };

    // 클래스 안에 기본형, enum, string, vector<클래스>, map을 모두 넣는다.
    struct Player
    {
        std::int32_t level{};
        Job job{};
        std::string name{};
        std::vector<Item> items{};
        std::map<std::string, std::int32_t> stats{};

        SERIALIZE_FIELDS(level, job, name, items, stats)
        bool operator==(const Player&) const = default;
    };
} // namespace

// 쓴 순서대로 읽으면 같은 값이 나와야 한다.
TEST(BinaryRW, PrimitivesRoundTrip)
{
    Base::BinaryWriter writer;
    writer << std::int8_t{-1} << std::uint16_t{65535} << std::int32_t{-123456} << std::uint64_t{1ull << 40};
    writer << 3.5f << -0.25 << true << false << Job::Archer;

    auto i8 = std::int8_t{};
    auto u16 = std::uint16_t{};
    auto i32 = std::int32_t{};
    auto u64 = std::uint64_t{};
    auto f = float{};
    auto d = double{};
    auto t = false;
    auto f2 = true;
    auto job = Job::Warrior;
    Base::BinaryReader reader(writer.Data());
    reader >> i8 >> u16 >> i32 >> u64 >> f >> d >> t >> f2 >> job;

    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(i8, -1);
    EXPECT_EQ(u16, 65535);
    EXPECT_EQ(i32, -123456);
    EXPECT_EQ(u64, 1ull << 40);
    EXPECT_EQ(f, 3.5f);
    EXPECT_EQ(d, -0.25);
    EXPECT_TRUE(t);
    EXPECT_FALSE(f2);
    EXPECT_EQ(job, Job::Archer);
    EXPECT_EQ(reader.ReadPos(), reader.Size());
}

// 형식이 C#과 같아야 하므로 바이트 배치를 직접 확인한다. 리틀 엔디안: 낮은 바이트가 먼저.
TEST(BinaryRW, LittleEndianLayout)
{
    Base::BinaryWriter writer;
    writer << std::uint32_t{0x12345678} << true << Job::Archer;

    const auto data = writer.Data();
    ASSERT_EQ(data.size(), 4u + 1u + 1u); // enum은 기반 타입(uint8_t) 크기
    EXPECT_EQ(data[0], std::byte{0x78});
    EXPECT_EQ(data[1], std::byte{0x56});
    EXPECT_EQ(data[2], std::byte{0x34});
    EXPECT_EQ(data[3], std::byte{0x12});
    EXPECT_EQ(data[4], std::byte{1});
    EXPECT_EQ(data[5], std::byte{2});
}

// string 형식: uint16 길이 + UTF-8 바이트. 한글은 글자당 3바이트.
TEST(BinaryRW, StringLayout)
{
    Base::BinaryWriter writer;
    writer << std::string{"AB"} << std::string{"가"} << std::string{};

    const auto data = writer.Data();
    ASSERT_EQ(data.size(), (2u + 2u) + (2u + 3u) + 2u);
    EXPECT_EQ(data[0], std::byte{2}); // 길이 2
    EXPECT_EQ(data[1], std::byte{0});
    EXPECT_EQ(data[2], std::byte{'A'});
    EXPECT_EQ(data[3], std::byte{'B'});
    EXPECT_EQ(data[4], std::byte{3}); // "가" = EA B0 80
    EXPECT_EQ(data[6], std::byte{0xEA});
}

// int, string, vector, map, 클래스(안에 vector<클래스>, map)를 한 번에 쓰고 같은 순서로 읽는다.
TEST(BinaryRW, EverythingRoundTrip)
{
    const auto number = std::int32_t{42};
    const auto text = std::string{"안녕하세요"};
    const auto numbers = std::vector<std::int32_t>{1, 2, 3};
    const auto names = std::vector<std::string>{"a", "", "다"};
    const auto table = std::map<std::int32_t, std::string>{{1, "one"}, {2, "two"}};
    const auto lookup = std::unordered_map<std::string, std::int32_t>{{"x", 10}, {"y", 20}};
    const auto player = Player{
        .level = 10,
        .job = Job::Archer,
        .name = "궁수",
        .items = {{1, "활"}, {2, "화살"}},
        .stats = {{"str", 5}, {"dex", 15}},
    };

    Base::BinaryWriter writer;
    writer << number << text << numbers << names << table << lookup << player;

    auto readNumber = std::int32_t{};
    auto readText = std::string{};
    auto readNumbers = std::vector<std::int32_t>{};
    auto readNames = std::vector<std::string>{};
    auto readTable = std::map<std::int32_t, std::string>{};
    auto readLookup = std::unordered_map<std::string, std::int32_t>{};
    auto readPlayer = Player{};
    Base::BinaryReader reader(writer.Data());
    reader >> readNumber >> readText >> readNumbers >> readNames >> readTable >> readLookup >> readPlayer;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readNumber, number);
    EXPECT_EQ(readText, text);
    EXPECT_EQ(readNumbers, numbers);
    EXPECT_EQ(readNames, names);
    EXPECT_EQ(readTable, table);
    EXPECT_EQ(readLookup, lookup);
    EXPECT_EQ(readPlayer, player);
    EXPECT_EQ(reader.ReadPos(), reader.Size()); // 남김없이 다 읽었다
}

// 받은 바이트 배열을 복사하지 않고 그대로 읽는다(네트워크에서 받은 경우).
TEST(BinaryRW, ReadFromReceivedBytes)
{
    const std::byte received[] = {std::byte{0x2A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Base::BinaryReader reader(received);

    auto value = std::int32_t{};
    reader >> value;

    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(value, 42);
}

// 0/1이 아닌 바이트가 와도 bool은 true/false 중 하나가 된다.
TEST(BinaryRW, BoolFromAnyNonZeroByte)
{
    const std::byte received[] = {std::byte{0x7F}};
    Base::BinaryReader reader(received);

    auto value = false;
    reader >> value;

    EXPECT_TRUE(value);
}

// 데이터가 모자라면 실패 표시가 남고, 값은 0이 되며, 이후 읽기도 무시된다.
TEST(BinaryRW, ReadPastEndFails)
{
    Base::BinaryWriter writer;
    writer << std::uint16_t{7};

    auto tooBig = std::uint32_t{99};
    auto after = std::uint8_t{99};
    Base::BinaryReader reader(writer.Data());
    reader >> tooBig >> after;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_EQ(tooBig, 0u);
    EXPECT_EQ(after, 0u);
    EXPECT_EQ(reader.ReadPos(), 0u); // 실패한 읽기는 위치를 움직이지 않는다
}

// 개수가 남은 바이트보다 크면(거짓 개수) 메모리를 잡지 않고 바로 실패한다.
TEST(BinaryRW, CountLargerThanRemainingFails)
{
    const std::byte received[] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{1}}; // 개수 65535, 실제 원소 1바이트
    Base::BinaryReader reader(received);

    auto values = std::vector<std::uint8_t>{9};
    reader >> values;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_TRUE(values.empty());
}

// 클래스 중간에서 데이터가 끊겨도 실패로 끝난다.
TEST(BinaryRW, TruncatedObjectFails)
{
    Base::BinaryWriter writer;
    writer << Player{.level = 1, .name = "전사", .items = {{1, "검"}}};
    const auto data = writer.Data();

    Base::BinaryReader reader(data.first(data.size() - 1)); // 마지막 1바이트 잘림
    auto player = Player{};
    reader >> player;

    EXPECT_FALSE(reader.IsValid());
}

// 길이가 uint16을 넘으면 쓰기가 실패하고, 이후 쓰기도 무시된다.
TEST(BinaryRW, TooLongStringFailsWrite)
{
    Base::BinaryWriter writer;
    writer << std::string(Base::BinaryWriter::MaxCount + 1, 'a') << std::int32_t{1};

    EXPECT_FALSE(writer.IsValid());
    EXPECT_EQ(writer.Size(), 0u);
}

// Clear는 내용과 실패 표시를 지우지만 확보한 메모리는 유지한다(재사용, 풀링 대비).
TEST(BinaryRW, ClearKeepsCapacityForReuse)
{
    Base::BinaryWriter writer;
    for (auto i = 0; i < 100; ++i)
    {
        writer << std::numeric_limits<std::uint64_t>::max();
    }
    writer << std::string(Base::BinaryWriter::MaxCount + 1, 'a'); // 실패 상태 만들기
    ASSERT_FALSE(writer.IsValid());

    const auto capacityBefore = writer.Capacity();
    ASSERT_GE(capacityBefore, 100u * sizeof(std::uint64_t));

    writer.Clear();

    EXPECT_EQ(writer.Capacity(), capacityBefore);
    EXPECT_EQ(writer.Size(), 0u);
    EXPECT_TRUE(writer.IsValid());
}

// 중복 키 컨테이너(multimap)도 왕복된다. (Codex 리뷰 R1)
TEST(BinaryRW, MultimapRoundTrip)
{
    const auto drops = std::multimap<std::int32_t, std::string>{{1, "검"}, {1, "방패"}, {2, "활"}};

    Base::BinaryWriter writer;
    writer << drops;

    auto readDrops = std::multimap<std::int32_t, std::string>{};
    Base::BinaryReader reader(writer.Data());
    reader >> readDrops;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readDrops, drops);
}
