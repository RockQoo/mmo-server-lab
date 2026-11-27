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

// ByteWriter와 ByteReader가 함께 쓰는 형식 규칙과 타입 판별.
//
// 형식 (리틀 엔디안, 길이·개수는 uint16)
//   bool                       1바이트 0/1
//   정수, 실수, enum            크기 그대로
//   문자열(모든 종류)            길이 + UTF-8 바이트. wstring/u16string/u32string은 UTF-8로 바꿔 쓴다.
//                              읽을 때 올바른 UTF-8인지 검사한다. 바이너리 데이터는 vector<std::byte>에 담는다
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
    static_assert(std::endian::native == std::endian::little, "ByteWriter/Reader는 리틀 엔디안 CPU만 지원한다");

    // string 길이, 컨테이너 개수의 상한(uint16)
    inline constexpr std::size_t ByteMaxCount{std::numeric_limits<uint16_t>::max()};

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

    // C 문자열 포인터. 쓰기만 지원한다(읽기는 std::string으로).
    template <typename T>
    concept CharPointer = std::is_same_v<T, const char*> || std::is_same_v<T, char*>;

    // UTF-8로 바꿔 쓰는 와이드 문자열
    template <typename C>
    concept WideChar = std::is_same_v<C, wchar_t> || std::is_same_v<C, char16_t> || std::is_same_v<C, char32_t>;

    // 문자 배열 = 문자열 리터럴("abc", u8"abc", L"abc", u"abc", U"abc")과 고정 문자 버퍼.
    // 고정 배열이 아니라 문자열로 쓴다(널 문자 앞까지). 쓰기만 지원한다(읽기는 std::string 등으로).
    template <typename T>
    concept CharArray = std::is_bounded_array_v<T> && (ByteChar<std::remove_cv_t<std::remove_extent_t<T>>> ||
                                                       WideChar<std::remove_cv_t<std::remove_extent_t<T>>>);
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

    // ---- 통째 복사(blittable): 메모리를 그대로 복사한 바이트가 필드마다 쓴 바이트와 같은 타입
    // 이런 타입의 배열(vector, array, T[N])은 원소마다 쓰지 않고 memcpy 한 번으로 쓰고 읽는다
    // (performance.html 사례 10).
    //   - bool 을 뺀 정수, 실수, enum (bool 은 읽을 때 0/1 검사가 필요하다)
    //   - SERIALIZE_FIELDS 클래스 중 ① 필드가 모두 blittable ② 필드를 선언 순서대로 적었고 ③ 필드 크기 합 = sizeof
    //     (빠진 필드도 빈틈도 없다). ①~③은 컴파일할 때 검사한다. 하나라도 아니면 지금처럼 필드마다 쓴다(바이트는 같다).
    // 빠른 길을 꼭 타야 하는 타입은 static_assert(Base::Blittable<T>); 를 붙인다(구조가 바뀌면 빌드 에러).
    namespace Detail
    {
        template <typename T>
        consteval bool IsBlittable();
    } // namespace Detail

    template <typename T>
    concept Blittable = Detail::IsBlittable<T>();

    namespace Detail
    {
        // SERIALIZE_FIELDS 가 만든 IsBlittableLayout() 이 부른다.
        // 주소가 적은 순서대로 커지고 크기 합이 sizeof 와 같은지.
        template <typename Self, typename... Fields>
        consteval bool IsPackedInOrder(const Self& /*self*/, const Fields&... fields)
        {
            if constexpr (!(Blittable<Fields> && ...) || (sizeof(Fields) + ...) != sizeof(Self))
            {
                return false;
            }
            else
            {
                const std::array<const void*, sizeof...(Fields)> addresses{{static_cast<const void*>(&fields)...}};
                for (auto i = std::size_t{1}; i < addresses.size(); ++i)
                {
                    if (!(addresses[i - 1] < addresses[i]))
                    {
                        return false;
                    }
                }
                return true;
            }
        }

        template <typename T>
        consteval bool IsBlittable()
        {
            if constexpr (Primitive<T>)
            {
                return !std::is_same_v<T, bool>;
            }
            // 기본 생성한 값을 컴파일 시간에 만들 수 있어야 검사할 수 있다. 못 만들면 그냥 아니라고 본다.
            else if constexpr (std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T> &&
                               requires { typename std::bool_constant<T{}.template IsBlittableLayout<T>()>; })
            {
                return T{}.template IsBlittableLayout<T>();
            }
            else
            {
                return false;
            }
        }
    } // namespace Detail
} // namespace Base

// 클래스 안에 적으면 Write/Read 멤버 함수를 만든다. 적은 순서가 곧 직렬화 순서다.
// 필드는 최소 1개. 쓰는 쪽에서 ByteWriter.h와 ByteReader.h를 include한다. (나중에 CodeGen으로 대체 예정)
//
//     struct Item
//     {
//         int32_t id{};
//         std::string name{};
//         SERIALIZE_FIELDS(id, name)
//     };
#define SERIALIZE_FIELDS(...)                                                                                          \
    void Write(Base::ByteWriter& writer) const                                                                         \
    {                                                                                                                  \
        writer.WriteFields(__VA_ARGS__);                                                                               \
    }                                                                                                                  \
    void Read(Base::ByteReader& reader)                                                                                \
    {                                                                                                                  \
        reader.ReadFields(__VA_ARGS__);                                                                                \
    }                                                                                                                  \
    /* Base::Blittable 검사용(템플릿이라 검사할 때만 만들어진다) */                                                    \
    template <typename Self>                                                                                           \
    consteval bool IsBlittableLayout() const                                                                           \
    {                                                                                                                  \
        return Base::Detail::IsPackedInOrder(static_cast<const Self&>(*this), __VA_ARGS__);                            \
    }
