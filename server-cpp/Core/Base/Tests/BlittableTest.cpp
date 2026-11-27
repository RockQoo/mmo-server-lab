#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Base/ByteReader.h"
#include "Base/ByteTraits.h"
#include "Base/ByteWriter.h"

// Base::Blittable: 통째로 복사해도 필드마다 쓴 것과 같은 바이트인 타입만 true.
// 판별은 컴파일할 때라 static_assert 로 본다.
namespace
{
    enum class Grade : uint8_t
    {
        Normal,
        Rare,
    };

    struct Slot // 빈틈 없음, 선언 순서대로
    {
        uint32_t itemId{};
        uint32_t count{};
        uint64_t uid{};
        SERIALIZE_FIELDS(itemId, count, uid)
    };

    struct Reordered // 적은 순서가 선언 순서와 다르다 → 통째 복사하면 바이트가 달라진다
    {
        uint32_t itemId{};
        uint32_t count{};
        uint64_t uid{};
        SERIALIZE_FIELDS(count, itemId, uid)
    };

    struct Missing // 필드 하나를 안 적었다
    {
        uint32_t itemId{};
        uint32_t count{};
        uint64_t uid{};
        SERIALIZE_FIELDS(itemId, count)
    };

    struct Duplicated // 같은 필드를 두 번(크기 합은 sizeof 와 같다)
    {
        uint32_t a{};
        uint32_t b{};
        SERIALIZE_FIELDS(a, a)
    };

    struct Padded // uint8 뒤에 빈틈 3바이트
    {
        uint8_t kind{};
        uint32_t value{};
        SERIALIZE_FIELDS(kind, value)
    };

    struct WithBool
    {
        uint32_t id{};
        bool locked{};
        uint8_t reserved1{};
        uint16_t reserved2{};
        SERIALIZE_FIELDS(id, locked, reserved1, reserved2)
    };

    struct Vec3 // float 도 된다
    {
        float x{};
        float y{};
        float z{};
        SERIALIZE_FIELDS(x, y, z)
    };

    struct Unit // 통째 복사되는 구조체를 품은 구조체, enum
    {
        Vec3 position{};
        Grade grade{};
        uint8_t level{};
        uint16_t hp{};
        SERIALIZE_FIELDS(position, grade, level, hp)
    };

    struct Named // string 은 메모리 그대로 복사할 수 없다
    {
        uint32_t id{};
        std::string name{};
        SERIALIZE_FIELDS(id, name)
    };

    static_assert(Base::Blittable<int32_t>);
    static_assert(Base::Blittable<double>);
    static_assert(Base::Blittable<Grade>);
    static_assert(!Base::Blittable<bool>);

    static_assert(Base::Blittable<Slot>);
    static_assert(Base::Blittable<Vec3>);
    static_assert(Base::Blittable<Unit>);
    static_assert(!Base::Blittable<Reordered>);
    static_assert(!Base::Blittable<Missing>);
    static_assert(!Base::Blittable<Duplicated>);
    static_assert(!Base::Blittable<Padded>);
    static_assert(!Base::Blittable<WithBool>);
    static_assert(!Base::Blittable<Named>);
} // namespace

namespace
{
    std::vector<std::byte> Bytes(const Base::ByteWriter& writer)
    {
        const auto data = writer.Data();
        return {data.begin(), data.end()};
    }
} // namespace

// 한 번에 쓴 바이트 = 필드마다 쓴 바이트(형식이 바뀌지 않는다. C# 과 주고받는 근거)
TEST(Blittable, BlockWriteMatchesFieldByFieldBytes)
{
    const auto slots = std::vector<Slot>{{.itemId = 1, .count = 2, .uid = 3}, {.itemId = 40, .count = 50, .uid = 60}};
    const auto units = std::array<Unit, 2>{
        Unit{.position = {1.5F, -2.0F, 3.25F}, .grade = Grade::Rare, .level = 7, .hp = 900}, Unit{}};

    Base::ByteWriter block;
    block << slots << units << slots[1];

    Base::ByteWriter fields;
    fields << static_cast<uint16_t>(slots.size());
    for (const auto& slot : slots)
    {
        fields << slot.itemId << slot.count << slot.uid;
    }
    fields << static_cast<uint16_t>(units.size());
    for (const auto& unit : units)
    {
        fields << unit.position.x << unit.position.y << unit.position.z << unit.grade << unit.level << unit.hp;
    }
    fields << slots[1].itemId << slots[1].count << slots[1].uid;

    ASSERT_TRUE(block.IsValid());
    EXPECT_EQ(Bytes(block), Bytes(fields));
}

TEST(Blittable, BlockReadRoundTrips)
{
    const auto slots = std::vector<Slot>{{.itemId = 1, .count = 2, .uid = 3}, {.itemId = 40, .count = 50, .uid = 60}};
    const Vec3 points[3] = {{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}, {7.0F, 8.0F, 9.0F}};
    Base::ByteWriter writer;
    writer << slots << points;

    auto readSlots = std::vector<Slot>{};
    Vec3 readPoints[3]{};
    Base::ByteReader reader{writer.Data()};
    reader >> readSlots >> readPoints;

    ASSERT_TRUE(reader.IsValid());
    ASSERT_EQ(readSlots.size(), 2U);
    EXPECT_EQ(readSlots[1].uid, 60U);
    EXPECT_EQ(readPoints[2].z, 9.0F);
}

// 개수만큼의 바이트가 없으면 늘리지 않고 실패, 빈 vector
TEST(Blittable, ShortBufferFailsWithoutGrowing)
{
    const auto slots = std::vector<Slot>(4);
    Base::ByteWriter writer;
    writer << slots;
    const auto data = writer.Data();

    auto read = std::vector<Slot>{};
    Base::ByteReader reader{data.first(data.size() - 1)}; // 마지막 1바이트가 없다
    reader >> read;

    EXPECT_FALSE(reader.IsValid());
    EXPECT_TRUE(read.empty());
    EXPECT_EQ(read.capacity(), 0U);
}
