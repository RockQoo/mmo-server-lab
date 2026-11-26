#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <format>
#include <type_traits>
#include <unordered_set>

#include "Base/ByteReader.h"
#include "Base/ByteWriter.h"
#include "Base/StrongId.h"

namespace
{
    using PlayerId = Base::StrongId<struct PlayerTag>;
    using GuildId = Base::StrongId<struct GuildTag>;
    using ItemDataId = Base::StrongId<struct ItemDataTag, std::int32_t>; // 정수 폭을 바꿀 수 있다

    // 막아야 하는 실수는 컴파일 자체가 안 된다.
    static_assert(!std::is_convertible_v<std::int64_t, PlayerId>, "정수가 ID로 몰래 바뀌면 안 된다");
    static_assert(!std::is_convertible_v<PlayerId, std::int64_t>, "ID가 정수로 몰래 바뀌면 안 된다");
    static_assert(!std::is_convertible_v<GuildId, PlayerId>, "다른 종류의 ID를 넣을 수 없다");
    static_assert(!std::equality_comparable_with<PlayerId, GuildId>, "다른 종류의 ID끼리 비교할 수 없다");

    // 비용은 정수와 같다: 크기가 같고, 복사가 memcpy와 같다(레지스터로 전달).
    static_assert(sizeof(PlayerId) == sizeof(std::int64_t));
    static_assert(sizeof(ItemDataId) == sizeof(std::int32_t));
    static_assert(std::is_trivially_copyable_v<PlayerId>);
} // namespace

TEST(StrongId, DefaultIsZero)
{
    EXPECT_EQ(PlayerId{}.Value(), 0);
}

TEST(StrongId, ComparesWithinSameKind)
{
    EXPECT_EQ(PlayerId{7}, PlayerId{7});
    EXPECT_NE(PlayerId{7}, PlayerId{8});
    EXPECT_LT(PlayerId{7}, PlayerId{8});
}

TEST(StrongId, WorksAsHashKey)
{
    std::unordered_set<PlayerId> ids{PlayerId{1}, PlayerId{2}, PlayerId{1}};
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.contains(PlayerId{2}));
}

TEST(StrongId, FormatsAsInteger)
{
    EXPECT_EQ(std::format("{}", PlayerId{42}), "42");
    EXPECT_EQ(std::format("{:>5}", PlayerId{42}), "   42");
}

// 직렬화하면 정수 하나와 같은 바이트다(C#도 같은 형식으로 읽는다).
TEST(StrongId, SerializesAsPlainInteger)
{
    Base::ByteWriter idWriter;
    idWriter << PlayerId{0x0102'0304'0506'0708};
    Base::ByteWriter intWriter;
    intWriter << std::int64_t{0x0102'0304'0506'0708};
    ASSERT_TRUE(idWriter.IsValid());
    EXPECT_TRUE(std::ranges::equal(idWriter.Data(), intWriter.Data()));

    Base::ByteReader reader{idWriter.Data()};
    PlayerId read{};
    reader >> read;
    ASSERT_TRUE(reader.IsValid());
    EXPECT_EQ(read, PlayerId{0x0102'0304'0506'0708});
}
