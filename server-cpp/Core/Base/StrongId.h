#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>

// 종류가 다른 ID끼리 섞이지 않게 하는 정수 ID 타입. Tag마다 다른 타입이 된다.
//
//     class Ruid : public StrongId<RuidTag> { ... };  // Base/Ruid.h. Ruid만의 멤버가 필요해 상속한다
//     using PlayerId = StrongId<struct PlayerTag>;    // 콘텐츠 ID(나중에 CodeGen이 만든다)
//     using GuildId  = StrongId<struct GuildTag>;
//
// 막는 실수
//   - 아무 정수를 ID 자리에 넣기: 생성자가 explicit이라 `PlayerId id = 5;`가 안 된다. `PlayerId{5}`처럼 적어야 한다.
//   - 다른 종류의 ID 넣기·비교하기: PlayerId 자리에 GuildId를 넣거나 둘을 ==로 비교하면 컴파일 에러다.
//   - ID로 산수하기: + - 같은 연산자가 없다.
//
// 비용: 멤버가 정수 하나라 크기와 복사 비용이 정수와 같고(레지스터로 전달), 비교·해시는 인라인돼 정수 비교와 같다.
// 직렬화도 정수 하나와 같은 바이트다(C#과 같은 형식).
//
// 0은 "없음"이다(기본 생성 값). 발급하는 쪽은 0을 내지 않는다(Ruid는 Realm 0을 예약해서 보장한다).
namespace Base
{
    template <typename Tag, std::integral T = int64_t>
    class StrongId
    {
    public:
        using ValueType = T;

        constexpr StrongId() noexcept = default;

        constexpr explicit StrongId(const T value) noexcept : _value{value}
        {
        }

        // 정수가 꼭 필요한 곳(DB 바인딩, 로그, 해시)에서만 꺼낸다.
        constexpr T Value() const noexcept
        {
            return _value;
        }

        // 같은 Tag끼리만 비교한다(==, <, 정렬, map 키).
        constexpr auto operator<=>(const StrongId&) const noexcept = default;

        // ByteWriter/ByteReader가 부른다(클래스 직렬화와 같은 경로). 정수 하나를 그대로 쓰고 읽는다.
        // 템플릿이라 이 헤더가 ByteWriter.h를 include하지 않아도 된다.
        template <typename Writer>
        void Write(Writer& writer) const
        {
            writer << _value;
        }

        template <typename Reader>
        void Read(Reader& reader)
        {
            reader >> _value;
        }

    private:
        T _value{};
    };
} // namespace Base

// 아래 두 특수화는 "우리 타입에 대한 std::hash·std::formatter 특수화"로, 표준이 허용하는 확장 방법이다.
// clang-tidy bugprone-std-namespace-modification 은 이것도 std 수정으로 보므로 그 줄에서만 끈다.

// unordered_map / unordered_set 키
template <typename Tag, typename T>
struct std::hash<Base::StrongId<Tag, T>> // NOLINT(bugprone-std-namespace-modification)
{
    std::size_t operator()(const Base::StrongId<Tag, T>& id) const noexcept
    {
        return std::hash<T>{}(id.Value());
    }
};

// std::format("{}", id) → 정수 값. 정수 서식({:>20} 등)도 그대로 된다.
template <typename Tag, typename T>
struct std::formatter<Base::StrongId<Tag, T>> : std::formatter<T> // NOLINT(bugprone-std-namespace-modification)
{
    // 이름은 표준(std::formatter)이 정한 것이라 Pascal로 바꿀 수 없다.
    // NOLINTNEXTLINE(readability-identifier-naming)
    auto format(const Base::StrongId<Tag, T>& id, std::format_context& context) const
    {
        return std::formatter<T>::format(id.Value(), context);
    }
};
