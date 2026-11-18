#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Base/BinaryTraits.h"

namespace Base
{
    class BinaryWriter;

    // SERIALIZE_FIELDS로 Write(BinaryWriter&)를 가진 클래스
    template <typename T>
    concept BinaryWritable = requires(const T& object, BinaryWriter& writer) { object.Write(writer); };

    // 값을 바이트로 쓴다(<<). 데이터는 자기 vector에 담는다(소유).
    // 실패하면(길이·개수 초과) 실패 표시를 남기고 이후 쓰기는 모두 무시한다. 보내기 전에 IsValid()로 확인한다.
    class BinaryWriter
    {
    public:
        static constexpr std::size_t MaxCount{BinaryMaxCount};

        // 지원하는 타입을 위에서부터 차례로 판별한다. 원소가 다시 이 함수를 불러 중첩이 된다.
        template <typename T>
        BinaryWriter& operator<<(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                *this << static_cast<std::uint8_t>(value ? 1 : 0);
            }
            else if constexpr (Primitive<T>)
            {
                WriteBytes(&value, sizeof(T));
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                WriteString(value);
            }
            else if constexpr (IsVector<T>)
            {
                WriteSequence(value);
            }
            else if constexpr (KeyValueContainer<T>)
            {
                WriteKeyValues(value);
            }
            else if constexpr (BinaryWritable<T>)
            {
                value.Write(*this);
            }
            else
            {
                static_assert(false, "BinaryWriter가 지원하지 않는 타입이다");
            }
            return *this;
        }

        // SERIALIZE_FIELDS가 부른다. 필드를 순서대로 쓴다(fold expression).
        template <typename... Ts>
        void WriteFields(const Ts&... fields)
        {
            (*this << ... << fields);
        }

        std::span<const std::byte> Data() const;
        std::size_t Size() const;
        std::size_t Capacity() const;
        bool IsValid() const;

        // 내용을 비우고 실패 표시를 지우되 확보한 메모리(capacity)는 유지한다. 재사용과 나중의 풀링을 위한 것.
        void Clear();

    private:
        void WriteBytes(const void* src, const std::size_t size);
        // 길이·개수를 uint16으로 쓴다. MaxCount를 넘으면 실패 표시 후 false.
        bool WriteCount(const std::size_t count);
        void WriteString(const std::string& value);

        template <typename C>
        void WriteSequence(const C& values)
        {
            if (WriteCount(values.size()))
            {
                for (const auto& value : values)
                {
                    *this << value;
                }
            }
        }

        template <typename M>
        void WriteKeyValues(const M& values)
        {
            if (WriteCount(values.size()))
            {
                for (const auto& [key, value] : values)
                {
                    *this << key << value;
                }
            }
        }

        std::vector<std::byte> _buffer{};
        bool _valid{true};
    };
} // namespace Base
