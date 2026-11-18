#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// BinaryWriter와 BinaryReader가 함께 쓰는 형식 규칙과 타입 판별.
//
// 형식 (리틀 엔디안, 길이·개수는 uint16)
//   bool                       1바이트 0/1
//   정수, 실수, enum            크기 그대로
//   문자열(모든 종류)            길이 + UTF-8 바이트. wstring/u16string/u32string은 UTF-8로 바꿔 쓴다
//   optional                   bool(있음) + 값
//   variant                    uint8 인덱스 + 값
//   pair, tuple                원소를 순서대로
//   chrono duration/time_point 틱 수(rep, 표준 단위는 int64)
//   고정 배열(array, T[N])       개수 + 원소. 읽을 때 개수가 다르면 실패
//   컨테이너                    개수 + 원소, map 계열은 개수 + (키, 값). 크기 0인 원소(monostate, 빈 tuple)는 안 됨
//   클래스                      SERIALIZE_FIELDS에 적은 순서대로
namespace Base
{
    // 형식은 리틀 엔디안 고정. x86/x64는 원래 리틀 엔디안이라 변환 없이 그대로 복사한다.
    static_assert(std::endian::native == std::endian::little, "BinaryWriter/Reader는 리틀 엔디안 CPU만 지원한다");

    // string 길이, 컨테이너 개수의 상한(uint16)
    inline constexpr std::size_t BinaryMaxCount{std::numeric_limits<std::uint16_t>::max()};

    // ---- 기본형: 메모리를 그대로 복사해도 되는 정수, 실수, enum(std::byte 포함). bool은 그보다 먼저 따로 잡는다.
    template <typename T>
    concept Primitive = std::is_arithmetic_v<T> || std::is_enum_v<T>;

    // ---- 문자열
    // 바이트 그대로 쓰는 UTF-8 문자열(string, u8string, pmr::string ... 할당자와 무관)
    template <typename C>
    concept ByteChar = std::is_same_v<C, char> || std::is_same_v<C, char8_t>;
    template <typename T>
    inline constexpr bool IsByteString = false;
    template <ByteChar C, typename Traits, typename Alloc>
    inline constexpr bool IsByteString<std::basic_string<C, Traits, Alloc>> = true;
    template <typename T>
    inline constexpr bool IsByteStringView = false;
    template <ByteChar C, typename Traits>
    inline constexpr bool IsByteStringView<std::basic_string_view<C, Traits>> = true;

    // 문자열 리터럴("abc")과 C 문자열 포인터. 쓰기만 지원한다(읽기는 std::string으로).
    template <typename T>
    concept CharArray = std::is_bounded_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>;
    template <typename T>
    concept CharPointer = std::is_same_v<T, const char*> || std::is_same_v<T, char*>;

    // UTF-8로 바꿔 쓰는 와이드 문자열
    template <typename C>
    concept WideChar = std::is_same_v<C, wchar_t> || std::is_same_v<C, char16_t> || std::is_same_v<C, char32_t>;
    template <typename T>
    inline constexpr bool IsWideString = false;
    template <WideChar C, typename Traits, typename Alloc>
    inline constexpr bool IsWideString<std::basic_string<C, Traits, Alloc>> = true;
    template <typename T>
    inline constexpr bool IsWideStringView = false;
    template <WideChar C, typename Traits>
    inline constexpr bool IsWideStringView<std::basic_string_view<C, Traits>> = true;

    // ---- 값 하나를 감싸는 타입
    template <typename T>
    inline constexpr bool IsOptional = false;
    template <typename T>
    inline constexpr bool IsOptional<std::optional<T>> = true;

    template <typename T>
    inline constexpr bool IsVariant = false;
    template <typename... Ts>
    inline constexpr bool IsVariant<std::variant<Ts...>> = true;

    template <typename T>
    inline constexpr bool IsTupleLike = false; // pair, tuple (std::array는 고정 배열로 따로 다룬다)
    template <typename A, typename B>
    inline constexpr bool IsTupleLike<std::pair<A, B>> = true;
    template <typename... Ts>
    inline constexpr bool IsTupleLike<std::tuple<Ts...>> = true;

    // 직렬화 크기가 0인 타입: monostate, 빈 tuple, 그런 것만 담은 tuple/pair.
    // 컨테이너 원소로는 쓸 수 없다. 읽기의 개수 검사가 "원소당 최소 1바이트"를 가정해 거짓 개수를 막기 때문이다.
    template <typename T>
    inline constexpr bool IsZeroSize = std::is_same_v<T, std::monostate>;
    template <typename... Ts>
    inline constexpr bool IsZeroSize<std::tuple<Ts...>> = (IsZeroSize<Ts> && ...);
    template <typename A, typename B>
    inline constexpr bool IsZeroSize<std::pair<A, B>> = IsZeroSize<A> && IsZeroSize<B>;

    template <typename T>
    inline constexpr bool IsDuration = false;
    template <typename Rep, typename Period>
    inline constexpr bool IsDuration<std::chrono::duration<Rep, Period>> = true;

    template <typename T>
    inline constexpr bool IsTimePoint = false;
    template <typename Clock, typename Duration>
    inline constexpr bool IsTimePoint<std::chrono::time_point<Clock, Duration>> = true;

    // ---- 컨테이너
    template <typename T>
    inline constexpr bool IsStdArray = false;
    template <typename T, std::size_t N>
    inline constexpr bool IsStdArray<std::array<T, N>> = true;

    // 크기가 컴파일 시간에 정해진 배열: std::array, T[N]
    template <typename T>
    concept FixedArray = IsStdArray<T> || std::is_bounded_array_v<T>;

    // 키-값 컨테이너: map, unordered_map, multimap, unordered_multimap
    template <typename T>
    concept KeyValueContainer = requires {
        typename T::key_type;
        typename T::mapped_type;
    };

    // 쓰기: 크기를 아는 range면 무엇이든(vector, list, deque, set, span ...)
    template <typename T>
    concept WritableSequence = std::ranges::sized_range<T>;

    // 읽기: 비우고 하나씩 넣을 수 있는 컨테이너(vector, list, deque, set, unordered_set, multiset ...)
    template <typename T>
    concept ReadableSequence = std::ranges::sized_range<T> && requires(T& container) {
        typename T::value_type;
        container.clear();
    };
} // namespace Base

// 클래스 안에 적으면 Write/Read 멤버 함수를 만든다. 적은 순서가 곧 직렬화 순서다.
// 필드는 최소 1개. 쓰는 쪽에서 BinaryWriter.h와 BinaryReader.h를 include한다. (나중에 CodeGen으로 대체 예정)
//
//     struct Item
//     {
//         std::int32_t id{};
//         std::string name{};
//         SERIALIZE_FIELDS(id, name)
//     };
#define SERIALIZE_FIELDS(...)                                                                                          \
    void Write(Base::BinaryWriter& writer) const                                                                       \
    {                                                                                                                  \
        writer.WriteFields(__VA_ARGS__);                                                                               \
    }                                                                                                                  \
    void Read(Base::BinaryReader& reader)                                                                              \
    {                                                                                                                  \
        reader.ReadFields(__VA_ARGS__);                                                                                \
    }
