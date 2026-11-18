#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

// BinaryWriter와 BinaryReader가 함께 쓰는 형식 규칙과 타입 판별.
//
// 형식: bool은 1바이트 0/1, 기본형(정수, 실수, enum)은 크기 그대로(리틀 엔디안),
//       string은 uint16 길이 + UTF-8 바이트, 컨테이너는 uint16 개수 + 원소들,
//       클래스는 SERIALIZE_FIELDS에 적은 순서대로.
namespace Base
{
    // 형식은 리틀 엔디안 고정. x86/x64는 원래 리틀 엔디안이라 변환 없이 그대로 복사한다.
    static_assert(std::endian::native == std::endian::little, "BinaryWriter/Reader는 리틀 엔디안 CPU만 지원한다");

    // string 길이, 컨테이너 개수의 상한(uint16)
    inline constexpr std::size_t BinaryMaxCount{std::numeric_limits<std::uint16_t>::max()};

    // 메모리를 그대로 복사해도 되는 기본형: 정수, 실수, enum (bool은 그보다 먼저 따로 잡는다)
    template <typename T>
    concept Primitive = std::is_arithmetic_v<T> || std::is_enum_v<T>;

    // vector (다음 조각에서 range 전체로 넓힌다)
    template <typename T>
    inline constexpr bool IsVector = false;
    template <typename T, typename A>
    inline constexpr bool IsVector<std::vector<T, A>> = true;

    // 키-값 컨테이너: map, unordered_map, multimap ...
    template <typename T>
    concept KeyValueContainer = requires {
        typename T::key_type;
        typename T::mapped_type;
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
