#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Base/ByteReader.h"
#include "Base/ByteWriter.h"

// C#(server-cs/Core/Base/Tests/ByteFormatTest.cs)과 같은 정답 파일(testdata/bytes/)을 쓴다.
// 한쪽 언어의 형식이 바뀌면 양쪽 테스트가 같이 깨진다. MMO_TESTDATA_DIR은 CMakeLists.txt가 넣는다.
namespace
{
    enum class Color : std::uint8_t
    {
        None = 0, // enum 은 0 값을 둔다({} 로 만든 값이 유효하게)
        Red = 1,
        Green = 2,
        Blue = 3,
    };

    // classes.hex 용. C# 테스트의 Item / Player(코덱 struct)와 필드 순서가 같다.
    enum class Job : std::uint8_t
    {
        None = 0, // enum 은 0 값을 둔다({} 로 만든 값이 유효하게)
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

    // "fe ff # 주석" 형식의 줄을 바이트로 바꾼다.
    std::vector<std::byte> LoadHex(const std::string& name)
    {
        auto file = std::ifstream{std::string{MMO_TESTDATA_DIR} + "/bytes/" + name};
        EXPECT_TRUE(file.is_open()) << name;

        auto bytes = std::vector<std::byte>{};
        auto line = std::string{};
        while (std::getline(file, line))
        {
            auto stream = std::istringstream{line.substr(0, line.find('#'))};
            auto token = std::string{};
            while (stream >> token)
            {
                bytes.push_back(static_cast<std::byte>(std::stoi(token, nullptr, 16)));
            }
        }
        return bytes;
    }
} // namespace

TEST(ByteFormat, BasicMatchesGoldenFile)
{
    const auto expected = LoadHex("basic.hex");
    ASSERT_FALSE(expected.empty());

    Base::ByteWriter writer;
    writer << std::int32_t{-2} << std::uint16_t{0x1234} << true << false << 1.5f << -2.25 << Color::Blue
           << std::int64_t{0x0102030405060708} << std::string{"가a"} << std::string{};
    ASSERT_TRUE(writer.IsValid());
    EXPECT_TRUE(std::ranges::equal(writer.Data(), expected));

    // 정답 바이트를 읽으면 같은 값이 나와야 한다.
    auto i32 = std::int32_t{};
    auto u16 = std::uint16_t{};
    auto yes = false;
    auto no = true;
    auto f = float{};
    auto d = double{};
    auto color = Color::Red;
    auto i64 = std::int64_t{};
    auto text = std::string{};
    auto empty = std::string{"x"};
    Base::ByteReader reader(expected);
    reader >> i32 >> u16 >> yes >> no >> f >> d >> color >> i64 >> text >> empty;

    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(reader.ReadPos(), expected.size());
    EXPECT_EQ(i32, -2);
    EXPECT_EQ(u16, 0x1234);
    EXPECT_TRUE(yes);
    EXPECT_FALSE(no);
    EXPECT_EQ(f, 1.5f);
    EXPECT_EQ(d, -2.25);
    EXPECT_EQ(color, Color::Blue);
    EXPECT_EQ(i64, 0x0102030405060708);
    EXPECT_EQ(text, "가a");
    EXPECT_EQ(empty, "");
}

TEST(ByteFormat, ContainersMatchGoldenFile)
{
    const auto expected = LoadHex("containers.hex");
    ASSERT_FALSE(expected.empty());

    using Bytes = std::vector<std::uint8_t>;
    Base::ByteWriter writer;
    writer << std::vector<std::int32_t>{1, 2, 3} << std::vector<std::int32_t>{} << std::vector<std::string>{"a", "가"}
           << std::optional<std::int32_t>{7} << std::optional<std::int32_t>{} << std::optional<std::string>{"x"}
           << std::vector<std::optional<std::int16_t>>{std::int16_t{5}, std::nullopt} << std::vector<Bytes>{{1, 2}, {}};
    ASSERT_TRUE(writer.IsValid());
    EXPECT_TRUE(std::ranges::equal(writer.Data(), expected));

    auto ints = std::vector<std::int32_t>{};
    auto none = std::vector<std::int32_t>{9};
    auto strings = std::vector<std::string>{};
    auto seven = std::optional<std::int32_t>{};
    auto missing = std::optional<std::int32_t>{1};
    auto x = std::optional<std::string>{};
    auto shorts = std::vector<std::optional<std::int16_t>>{};
    auto nested = std::vector<Bytes>{};
    Base::ByteReader reader(expected);
    reader >> ints >> none >> strings >> seven >> missing >> x >> shorts >> nested;

    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(reader.ReadPos(), expected.size());
    EXPECT_EQ(ints, (std::vector<std::int32_t>{1, 2, 3}));
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(strings, (std::vector<std::string>{"a", "가"}));
    EXPECT_EQ(seven, 7);
    EXPECT_FALSE(missing.has_value());
    EXPECT_EQ(x, "x");
    EXPECT_EQ(shorts, (std::vector<std::optional<std::int16_t>>{std::int16_t{5}, std::nullopt}));
    EXPECT_EQ(nested, (std::vector<Bytes>{{1, 2}, {}}));
}

TEST(ByteFormat, ClassesMatchGoldenFile)
{
    const auto expected = LoadHex("classes.hex");
    ASSERT_FALSE(expected.empty());

    const Player player{
        .level = 10,
        .job = Job::Archer,
        .name = "쿠",
        .items = {Item{.id = 1, .name = "검"}},
        .stats = {{"hp", 100}, {"mp", 50}},
    };
    Base::ByteWriter writer;
    writer << player;
    ASSERT_TRUE(writer.IsValid());
    EXPECT_TRUE(std::ranges::equal(writer.Data(), expected));

    auto read = Player{};
    Base::ByteReader reader(expected);
    reader >> read;
    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(reader.ReadPos(), expected.size());
    EXPECT_EQ(read, player);
}
