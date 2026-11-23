#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Base/ByteTraits.h"
#include "Base/Utf8.h"

namespace Base
{
    class ByteWriter;

    // SERIALIZE_FIELDS로 Write(ByteWriter&)를 가진 클래스
    template <typename T>
    concept ByteWritable = requires(const T& object, ByteWriter& writer) { object.Write(writer); };

    // 값을 바이트로 쓴다(<<). 데이터는 자기 vector에 담는다(소유). 형식은 ByteTraits.h 맨 위.
    // 실패하면(길이·개수 초과, 잘못된 UTF-16/32, null 문자열 포인터) 실패 표시를 남기고 이후 쓰기는 모두 무시한다.
    // 보내기 전에 IsValid()로 확인한다.
    class ByteWriter
    {
    public:
        static constexpr std::size_t MaxCount{ByteMaxCount};

        // 지원하는 타입을 위에서부터 차례로 판별한다. 원소가 다시 이 함수를 불러 중첩이 된다.
        template <typename T>
        ByteWriter& operator<<(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                *this << static_cast<std::uint8_t>(value ? 1 : 0);
            }
            else if constexpr (Primitive<T>)
            {
                WriteBytes(&value, sizeof(T));
            }
            else if constexpr (IsByteString<T> || IsByteStringView<T>)
            {
                WriteString(value.data(), value.size());
            }
            else if constexpr (CharArray<T>)
            {
                // 리터럴 "abc"는 char[4]다. 끝의 널 문자(없으면 배열 끝)까지만 쓴다.
                using Char = std::remove_cv_t<std::remove_extent_t<T>>;
                const auto* end = std::ranges::find(value, Char{});
                const auto text = std::basic_string_view<Char>{value, static_cast<std::size_t>(end - value)};
                if constexpr (ByteChar<Char>)
                {
                    WriteString(text.data(), text.size());
                }
                else
                {
                    WriteWideString(text);
                }
            }
            else if constexpr (CharPointer<T>)
            {
                WriteCString(value);
            }
            else if constexpr (IsWideString<T> || IsWideStringView<T>)
            {
                WriteWideString(std::basic_string_view<typename T::value_type>{value});
            }
            else if constexpr (IsOptional<T>)
            {
                *this << value.has_value();
                if (value.has_value())
                {
                    *this << *value;
                }
            }
            else if constexpr (IsVariant<T>)
            {
                WriteVariant(value);
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
                // variant의 "빈 값". 쓸 내용이 없다.
            }
            else if constexpr (IsTupleLike<T>)
            {
                std::apply([this](const auto&... elements) { WriteFields(elements...); }, value);
            }
            else if constexpr (IsDuration<T>)
            {
                *this << value.count();
            }
            else if constexpr (IsTimePoint<T>)
            {
                *this << value.time_since_epoch().count();
            }
            else if constexpr (ByteWritable<T>)
            {
                value.Write(*this);
            }
            else if constexpr (FixedArray<T>)
            {
                WriteSequence(value);
            }
            else if constexpr (KeyValueContainer<T>)
            {
                WriteKeyValues(value);
            }
            else if constexpr (WritableSequence<T>)
            {
                WriteSequence(value);
            }
            else
            {
                static_assert(false, "ByteWriter가 지원하지 않는 타입이다(포인터, 스마트 포인터, 함수 등)");
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
        // 길이 + 바이트. data는 UTF-8 문자열(char 또는 char8_t).
        void WriteString(const void* data, const std::size_t size);
        // null이면 실패
        void WriteCString(const char* value);

        template <typename C>
        void WriteWideString(const std::basic_string_view<C> value)
        {
            // UTF-8은 문자 단위 하나당 1바이트 이상이므로, 단위 수가 MaxCount를 넘으면 변환해 볼 필요 없이 초과다.
            if (!_valid || value.size() > MaxCount)
            {
                _valid = false;
                return;
            }
            auto utf8 = std::string{};
            if (!EncodeUtf8(value, utf8))
            {
                _valid = false;
                return;
            }
            WriteString(utf8.data(), utf8.size());
        }

        template <typename V>
        void WriteVariant(const V& value)
        {
            static_assert(std::variant_size_v<V> <= std::numeric_limits<std::uint8_t>::max(),
                          "variant 대안은 255개까지(인덱스를 uint8로 쓴다)");
            if (value.valueless_by_exception())
            {
                _valid = false;
                return;
            }
            *this << static_cast<std::uint8_t>(value.index());
            std::visit([this](const auto& alternative) { *this << alternative; }, value);
        }

        template <typename C>
        void WriteSequence(const C& values)
        {
            static_assert(!IsZeroSize<std::ranges::range_value_t<C>>,
                          "크기가 0인 원소(monostate, 빈 tuple)의 컨테이너는 지원하지 않는다");
            if (WriteCount(static_cast<std::size_t>(std::ranges::size(values))))
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
            static_assert(!(IsZeroSize<typename M::key_type> && IsZeroSize<typename M::mapped_type>),
                          "키와 값이 모두 크기 0인 컨테이너는 지원하지 않는다");
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
