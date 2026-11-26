#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "Base/ByteReader.h"
#include "Base/ByteWriter.h"

namespace
{
    enum class Job : uint8_t
    {
        None = 0, // enum 은 0 값을 둔다({} 로 만든 값이 유효하게)
        Warrior = 1,
        Archer = 2,
    };

    struct Item
    {
        int32_t id{};
        std::string name{};

        SERIALIZE_FIELDS(id, name)
        bool operator==(const Item&) const = default;
    };

    // 클래스 안에 기본형, enum, string, vector<클래스>, map을 모두 넣는다.
    struct Player
    {
        int32_t level{};
        Job job{};
        std::string name{};
        std::vector<Item> items{};
        std::map<std::string, int32_t> stats{};

        SERIALIZE_FIELDS(level, job, name, items, stats)
        bool operator==(const Player&) const = default;
    };
} // namespace

// 쓴 순서대로 읽으면 같은 값이 나와야 한다.
TEST(ByteRW, PrimitivesRoundTrip)
{
    Base::ByteWriter writer;
    writer << int8_t{-1} << uint16_t{65535} << int32_t{-123456} << uint64_t{1ull << 40};
    writer << 3.5f << -0.25 << true << false << Job::Archer;

    auto i8 = int8_t{};
    auto u16 = uint16_t{};
    auto i32 = int32_t{};
    auto u64 = uint64_t{};
    auto f = float{};
    auto d = double{};
    auto t = false;
    auto f2 = true;
    auto job = Job::Warrior;
    Base::ByteReader reader(writer.Data());
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
TEST(ByteRW, LittleEndianLayout)
{
    Base::ByteWriter writer;
    writer << uint32_t{0x12345678} << true << Job::Archer;

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
TEST(ByteRW, StringLayout)
{
    Base::ByteWriter writer;
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
TEST(ByteRW, EverythingRoundTrip)
{
    const auto number = int32_t{42};
    const auto text = std::string{"안녕하세요"};
    const auto numbers = std::vector<int32_t>{1, 2, 3};
    const auto names = std::vector<std::string>{"a", "", "다"};
    const auto table = std::map<int32_t, std::string>{{1, "one"}, {2, "two"}};
    const auto lookup = std::unordered_map<std::string, int32_t>{{"x", 10}, {"y", 20}};
    const Player player{
        .level = 10,
        .job = Job::Archer,
        .name = "궁수",
        .items = {{1, "활"}, {2, "화살"}},
        .stats = {{"str", 5}, {"dex", 15}},
    };

    Base::ByteWriter writer;
    writer << number << text << numbers << names << table << lookup << player;

    auto readNumber = int32_t{};
    auto readText = std::string{};
    auto readNumbers = std::vector<int32_t>{};
    auto readNames = std::vector<std::string>{};
    auto readTable = std::map<int32_t, std::string>{};
    auto readLookup = std::unordered_map<std::string, int32_t>{};
    auto readPlayer = Player{};
    Base::ByteReader reader(writer.Data());
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
TEST(ByteRW, ReadFromReceivedBytes)
{
    const std::byte received[] = {std::byte{0x2A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    Base::ByteReader reader(received);

    auto value = int32_t{};
    reader >> value;

    EXPECT_TRUE(reader.IsValid());
    EXPECT_EQ(value, 42);
}

// 0/1이 아닌 바이트가 와도 bool은 true/false 중 하나가 된다.
TEST(ByteRW, BoolFromAnyNonZeroByte)
{
    const std::byte received[] = {std::byte{0x7F}};
    Base::ByteReader reader(received);

    auto value = false;
    reader >> value;

    EXPECT_TRUE(value);
}

// 데이터가 모자라면 실패 표시가 남고, 값은 0이 되며, 이후 읽기도 무시된다.
TEST(ByteRW, ReadPastEndFails)
{
    Base::ByteWriter writer;
    writer << uint16_t{7};

    auto tooBig = uint32_t{99};
    auto after = uint8_t{99};
    Base::ByteReader reader(writer.Data());
    reader >> tooBig >> after;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_EQ(tooBig, 0u);
    EXPECT_EQ(after, 0u);
    EXPECT_EQ(reader.ReadPos(), 0u); // 실패한 읽기는 위치를 움직이지 않는다
}

// 개수가 남은 바이트보다 크면(거짓 개수) 메모리를 잡지 않고 바로 실패한다.
TEST(ByteRW, CountLargerThanRemainingFails)
{
    const std::byte received[] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{1}}; // 개수 65535, 실제 원소 1바이트
    Base::ByteReader reader(received);

    auto values = std::vector<uint8_t>{9};
    reader >> values;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_TRUE(values.empty());
}

// 클래스 중간에서 데이터가 끊겨도 실패로 끝난다.
TEST(ByteRW, TruncatedObjectFails)
{
    Base::ByteWriter writer;
    writer << Player{.level = 1, .name = "전사", .items = {{1, "검"}}};
    const auto data = writer.Data();

    Base::ByteReader reader(data.first(data.size() - 1)); // 마지막 1바이트 잘림
    auto player = Player{};
    reader >> player;

    EXPECT_FALSE(reader.IsValid());
}

// 길이가 uint16을 넘으면 쓰기가 실패하고, 이후 쓰기도 무시된다.
TEST(ByteRW, TooLongStringFailsWrite)
{
    Base::ByteWriter writer;
    writer << std::string(Base::ByteWriter::MaxCount + 1, 'a') << int32_t{1};

    EXPECT_FALSE(writer.IsValid());
    EXPECT_EQ(writer.Size(), 0u);
}

// Clear는 내용과 실패 표시를 지우지만 확보한 메모리는 유지한다(재사용, 풀링 대비).
TEST(ByteRW, ClearKeepsCapacityForReuse)
{
    Base::ByteWriter writer;
    for (auto i = 0; i < 100; ++i)
    {
        writer << std::numeric_limits<uint64_t>::max();
    }
    writer << std::string(Base::ByteWriter::MaxCount + 1, 'a'); // 실패 상태 만들기
    ASSERT_FALSE(writer.IsValid());

    const auto capacityBefore = writer.Capacity();
    ASSERT_GE(capacityBefore, 100u * sizeof(uint64_t));

    writer.Clear();

    EXPECT_EQ(writer.Capacity(), capacityBefore);
    EXPECT_EQ(writer.Size(), 0u);
    EXPECT_TRUE(writer.IsValid());
}

// 중복 키 컨테이너(multimap)도 왕복된다. (Codex 리뷰 R1)
TEST(ByteRW, MultimapRoundTrip)
{
    const auto drops = std::multimap<int32_t, std::string>{{1, "검"}, {1, "방패"}, {2, "활"}};

    Base::ByteWriter writer;
    writer << drops;

    auto readDrops = std::multimap<int32_t, std::string>{};
    Base::ByteReader reader(writer.Data());
    reader >> readDrops;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readDrops, drops);
}

// ---- ② 문자열 종류, 컨테이너 일반화, 고정 배열, pair/tuple

// 리터럴, C 문자열, string_view, u8string은 모두 같은 형식(길이 + UTF-8)이라 std::string으로 읽힌다.
TEST(ByteRW, StringKindsWriteSameFormat)
{
    const char buffer[8] = "ab"; // 고정 버퍼: 널 문자 앞까지만
    const char* pointer = "cd";

    Base::ByteWriter writer;
    writer << "리터럴" << pointer << std::string_view{"view"} << std::u8string{u8"유니"} << buffer;

    auto literal = std::string{};
    auto fromPointer = std::string{};
    auto fromView = std::string{};
    auto fromU8 = std::u8string{};
    auto fromBuffer = std::string{};
    Base::ByteReader reader(writer.Data());
    reader >> literal >> fromPointer >> fromView >> fromU8 >> fromBuffer;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(literal, "리터럴");
    EXPECT_EQ(fromPointer, "cd");
    EXPECT_EQ(fromView, "view");
    EXPECT_TRUE(fromU8 == u8"유니"); // vcpkg gtest(C++17 빌드)에 u8string 출력 함수가 없어 EXPECT_EQ 대신
    EXPECT_EQ(fromBuffer, "ab");
    EXPECT_EQ(reader.ReadPos(), reader.Size());
}

TEST(ByteRW, NullCStringFailsWrite)
{
    const char* nothing = nullptr;
    Base::ByteWriter writer;
    writer << nothing;

    EXPECT_FALSE(writer.IsValid());
}

// wstring, u16string, u32string은 UTF-8로 바꿔 쓴다. 그래서 바이트가 같은 내용의 std::string과 똑같다.
// 😀(U+1F600)는 UTF-16에서 서로게이트 쌍, UTF-8에서 4바이트.
TEST(ByteRW, WideStringsAreWrittenAsUtf8)
{
    Base::ByteWriter utf8Writer;
    utf8Writer << std::string{"안녕😀"};

    Base::ByteWriter wideWriter;
    wideWriter << std::wstring{L"안녕😀"} << std::u16string{u"안녕😀"} << std::u32string{U"안녕😀"};

    const auto one = utf8Writer.Data();
    const auto all = wideWriter.Data();
    ASSERT_EQ(all.size(), one.size() * 3);
    for (auto i = std::size_t{}; i < 3; ++i)
    {
        EXPECT_TRUE(std::ranges::equal(all.subspan(i * one.size(), one.size()), one));
    }

    auto wide = std::wstring{};
    auto utf16 = std::u16string{};
    auto utf32 = std::u32string{};
    Base::ByteReader reader(all);
    reader >> wide >> utf16 >> utf32;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(wide, L"안녕😀");
    EXPECT_EQ(utf16, u"안녕😀");
    EXPECT_EQ(utf32, U"안녕😀");
}

// 짝이 없는 서로게이트는 UTF-8로 바꿀 수 없으므로 쓰기 실패.
TEST(ByteRW, BrokenUtf16FailsWrite)
{
    const auto lonelyHigh = std::u16string{u'A', static_cast<char16_t>(0xD83D)};
    Base::ByteWriter writer;
    writer << lonelyHigh;

    EXPECT_FALSE(writer.IsValid());
}

// 잘못된 UTF-8(overlong: 슬래시를 2바이트 C0 AF로 쓴 것)은 wstring으로 읽을 때 실패.
TEST(ByteRW, BrokenUtf8FailsWideRead)
{
    const std::byte received[] = {std::byte{2}, std::byte{0}, std::byte{0xC0}, std::byte{0xAF}};
    Base::ByteReader reader(received);

    auto value = std::wstring{L"x"};
    reader >> value;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_TRUE(value.empty());
}

// 크기를 아는 range는 모두 같은 형식(개수 + 원소)이라 서로 바꿔 읽을 수 있다.
TEST(ByteRW, SequenceContainersRoundTrip)
{
    const auto list = std::list<int32_t>{3, 1, 2};
    const auto deque = std::deque<std::string>{"a", "b"};
    const auto set = std::set<int32_t>{5, 1, 3};
    const auto multiset = std::multiset<int32_t>{1, 1, 2};
    const auto unorderedSet = std::unordered_set<std::string>{"x", "y"};
    const auto flags = std::vector<bool>{true, false, true};
    const int32_t raw[] = {7, 8, 9};

    Base::ByteWriter writer;
    writer << list << deque << set << multiset << unorderedSet << flags << std::span{raw};

    auto readList = std::list<int32_t>{};
    auto readDeque = std::deque<std::string>{};
    auto readSet = std::set<int32_t>{};
    auto readMultiset = std::multiset<int32_t>{};
    auto readUnorderedSet = std::unordered_set<std::string>{};
    auto readFlags = std::vector<bool>{};
    auto fromSpan = std::vector<int32_t>{}; // span으로 쓴 것을 vector로 읽는다
    Base::ByteReader reader(writer.Data());
    reader >> readList >> readDeque >> readSet >> readMultiset >> readUnorderedSet >> readFlags >> fromSpan;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readList, list);
    EXPECT_EQ(readDeque, deque);
    EXPECT_EQ(readSet, set);
    EXPECT_EQ(readMultiset, multiset);
    EXPECT_EQ(readUnorderedSet, unorderedSet);
    EXPECT_EQ(readFlags, flags);
    EXPECT_EQ(fromSpan, (std::vector<int32_t>{7, 8, 9}));
}

// 고정 배열도 개수를 쓰고(안전), 읽을 때 크기가 다르면 실패한다.
TEST(ByteRW, FixedArraysRoundTripAndCheckCount)
{
    const auto stats = std::array<int16_t, 3>{{10, 20, 30}};
    const uint8_t slots[2] = {4, 5};

    Base::ByteWriter writer;
    writer << stats << slots;

    auto readStats = std::array<int16_t, 3>{};
    uint8_t readSlots[2] = {};
    Base::ByteReader reader(writer.Data());
    reader >> readStats >> readSlots;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readStats, stats);
    EXPECT_EQ(readSlots[0], 4);
    EXPECT_EQ(readSlots[1], 5);

    auto wrongSize = std::array<int16_t, 4>{{1, 1, 1, 1}};
    Base::ByteReader mismatch(writer.Data());
    mismatch >> wrongSize;

    EXPECT_FALSE(mismatch.IsValid());
    EXPECT_EQ(wrongSize, (std::array<int16_t, 4>{}));
}

TEST(ByteRW, PairAndTupleRoundTrip)
{
    const auto position = std::pair<float, float>{1.5f, -2.0f};
    const auto record = std::tuple<int32_t, std::string, bool>{7, "기록", true};

    Base::ByteWriter writer;
    writer << position << record;

    auto readPosition = std::pair<float, float>{};
    auto readRecord = std::tuple<int32_t, std::string, bool>{};
    Base::ByteReader reader(writer.Data());
    reader >> readPosition >> readRecord;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readPosition, position);
    EXPECT_EQ(readRecord, record);
}

// ---- ③ optional, variant, chrono

TEST(ByteRW, OptionalRoundTrip)
{
    const auto some = std::optional<std::string>{"있음"};
    const auto none = std::optional<int32_t>{};

    Base::ByteWriter writer;
    writer << some << none;

    auto readSome = std::optional<std::string>{};
    auto readNone = std::optional<int32_t>{99};
    Base::ByteReader reader(writer.Data());
    reader >> readSome >> readNone;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readSome, some);
    EXPECT_FALSE(readNone.has_value());
    EXPECT_EQ(writer.Size(), (1u + 2u + 6u) + 1u); // 있음: bool + 길이 + "있음"(6바이트), 없음: bool만
}

TEST(ByteRW, VariantRoundTrip)
{
    using Reward = std::variant<std::monostate, int32_t, std::string>;
    const auto values = std::vector<Reward>{Reward{}, Reward{100}, Reward{"전설 검"}};

    Base::ByteWriter writer;
    writer << values;

    auto readValues = std::vector<Reward>{};
    Base::ByteReader reader(writer.Data());
    reader >> readValues;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readValues, values);
}

// 없는 대안 번호가 오면 실패한다.
TEST(ByteRW, VariantBadIndexFails)
{
    const std::byte received[] = {std::byte{5}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    Base::ByteReader reader(received);

    auto value = std::variant<int32_t, float>{3};
    reader >> value;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_EQ(value.index(), 0u);
    EXPECT_EQ(std::get<0>(value), 0);
}

TEST(ByteRW, ChronoRoundTrip)
{
    using namespace std::chrono_literals;
    const auto cooldown = std::chrono::milliseconds{1500ms};
    const auto loginTime = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};

    Base::ByteWriter writer;
    writer << cooldown << loginTime;

    auto readCooldown = std::chrono::milliseconds{};
    auto readLoginTime = std::chrono::system_clock::time_point{};
    Base::ByteReader reader(writer.Data());
    reader >> readCooldown >> readLoginTime;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readCooldown, cooldown);
    EXPECT_EQ(readLoginTime, loginTime);
    EXPECT_EQ(writer.Size(), 8u + 8u); // 둘 다 int64 틱 수
}

// 지원 타입을 깊게 섞어도 왕복된다.
TEST(ByteRW, DeeplyNestedRoundTrip)
{
    using Bag = std::map<std::string, std::vector<std::optional<Item>>>;
    using Places = std::unordered_map<int32_t, std::tuple<std::wstring, std::array<float, 2>>>;
    const auto bag = Bag{
        {"무기", {Item{1, "검"}, std::nullopt, Item{2, "활"}}},
        {"빈칸", {}},
    };
    const auto places = Places{
        {1, {L"마을", {{0.f, 1.f}}}},
    };

    Base::ByteWriter writer;
    writer << bag << places;

    auto readBag = Bag{};
    auto readPlaces = Places{};
    Base::ByteReader reader(writer.Data());
    reader >> readBag >> readPlaces;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readBag, bag);
    EXPECT_EQ(readPlaces, places);
}

// ---- Codex 리뷰 반영

// R1: 와이드 문자열 단위 수가 MaxCount를 넘으면 UTF-8로 바꾸기 전에 바로 실패한다.
TEST(ByteRW, TooLongWideStringFailsBeforeConvert)
{
    Base::ByteWriter writer;
    writer << std::wstring(Base::ByteWriter::MaxCount + 1, L'a');

    EXPECT_FALSE(writer.IsValid());
    EXPECT_EQ(writer.Size(), 0u);
}

// R3: 다차원 C 배열도 읽히고, 실패하면 모든 원소가 0이 된다.
TEST(ByteRW, MultiDimensionalArrayRoundTrip)
{
    const int32_t grid[2][2] = {{1, 2}, {3, 4}};
    Base::ByteWriter writer;
    writer << grid;

    int32_t readGrid[2][2] = {};
    Base::ByteReader reader(writer.Data());
    reader >> readGrid;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readGrid[1][0], 3);

    const auto data = writer.Data();
    int32_t cutGrid[2][2] = {{9, 9}, {9, 9}};
    Base::ByteReader cut(data.first(data.size() - 1));
    cut >> cutGrid;

    EXPECT_FALSE(cut.IsValid());
    EXPECT_EQ(cutGrid[0][0], 0);
    EXPECT_EQ(cutGrid[1][1], 0);
}

// R4: 할당자가 다른 문자열(pmr)도 같은 형식으로 쓰고 읽는다.
TEST(ByteRW, PmrStringsRoundTrip)
{
    auto pool = std::pmr::monotonic_buffer_resource{};
    const auto name = std::pmr::string{"이름", &pool};
    const auto wide = std::pmr::wstring{L"넓은", &pool};

    Base::ByteWriter writer;
    writer << name << wide;

    auto readName = std::pmr::string{&pool};
    auto readWide = std::pmr::wstring{&pool};
    Base::ByteReader reader(writer.Data());
    reader >> readName >> readWide;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readName, name);
    EXPECT_EQ(readWide, wide);
}

// ---- std::string 읽기의 UTF-8 검사

namespace
{
    // 길이(uint16) + 바이트로 된 패킷 하나를 만든다.
    std::vector<std::byte> MakeStringPacket(const std::vector<uint8_t>& bytes)
    {
        auto packet = std::vector<std::byte>{std::byte{static_cast<uint8_t>(bytes.size())}, std::byte{0}};
        for (const auto b : bytes)
        {
            packet.push_back(std::byte{b});
        }
        return packet;
    }
} // namespace

// 잘못된 UTF-8은 std::string으로 읽을 때 실패한다.
TEST(ByteRW, BrokenUtf8FailsStringRead)
{
    const auto broken = std::vector<std::vector<uint8_t>>{
        {0xC0, 0xAF},             // overlong 슬래시 (필터 우회 공격에 쓰임)
        {0x80},                   // 앞 바이트 없이 이어지는 바이트만
        {0xEA, 0xB0},             // 3바이트 문자가 잘림
        {0xED, 0xA0, 0x80},       // UTF-16 서로게이트 영역(UTF-8에서 금지)
        {0xF4, 0x90, 0x80, 0x80}, // 0x10FFFF 초과
        {0xFF},                   // UTF-8에 없는 바이트
    };
    for (const auto& bytes : broken)
    {
        const auto packet = MakeStringPacket(bytes);
        Base::ByteReader reader(packet);
        auto value = std::string{"x"};
        reader >> value;

        EXPECT_FALSE(reader.IsValid());
        EXPECT_TRUE(value.empty());
    }
}

// 올바른 UTF-8(ASCII, 한글 3바이트, 이모지 4바이트)은 통과한다.
TEST(ByteRW, ValidUtf8PassesStringRead)
{
    const auto packet = MakeStringPacket({'A', 0xEA, 0xB0, 0x80, 0xF0, 0x9F, 0x98, 0x80}); // "A가😀"
    Base::ByteReader reader(packet);
    auto value = std::string{};
    reader >> value;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(value, "A가😀");
}

// 텍스트가 아닌 바이너리는 vector<std::byte>에 담으면 검사 없이 그대로 왕복된다.
TEST(ByteRW, BinaryBlobUsesByteVector)
{
    const auto blob = std::vector<std::byte>{std::byte{0xFF}, std::byte{0xC0}, std::byte{0x00}};
    Base::ByteWriter writer;
    writer << blob;

    auto readBlob = std::vector<std::byte>{};
    Base::ByteReader reader(writer.Data());
    reader >> readBlob;

    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(readBlob, blob);
}

// ---- Codex 리뷰 2차 반영

// R2: u8/L/u/U 리터럴도 문자열로 쓴다(고정 배열로 쓰면 널 문자와 원시 코드 단위가 들어가 다음 필드가 어긋난다).
TEST(ByteRW, UnicodeLiteralsWriteAsStrings)
{
    Base::ByteWriter fromLiterals;
    fromLiterals << u8"가" << L"가" << u"가" << U"가" << int8_t{7};

    Base::ByteWriter fromStrings;
    fromStrings << std::string{"가"} << std::string{"가"} << std::string{"가"} << std::string{"가"} << int8_t{7};

    ASSERT_TRUE(fromLiterals.IsValid());
    EXPECT_TRUE(std::ranges::equal(fromLiterals.Data(), fromStrings.Data()));
}

// R1: C 문자열은 MaxCount까지는 쓰고, 넘으면 실패한다(끝까지 훑지 않는다).
TEST(ByteRW, CStringLengthLimit)
{
    const auto exact = std::string(Base::ByteWriter::MaxCount, 'a');
    const auto tooLong = std::string(Base::ByteWriter::MaxCount + 1, 'a');

    Base::ByteWriter ok;
    ok << exact.c_str();
    EXPECT_TRUE(ok.IsValid());
    EXPECT_EQ(ok.Size(), 2u + Base::ByteWriter::MaxCount);

    Base::ByteWriter fail;
    fail << tooLong.c_str();
    EXPECT_FALSE(fail.IsValid());
    EXPECT_EQ(fail.Size(), 0u);
}
