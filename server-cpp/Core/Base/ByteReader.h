#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Base/ByteTraits.h"
#include "Base/Utf8.h"

namespace Base
{
    class ByteReader;

    // SERIALIZE_FIELDS로 Read(ByteReader&)를 가진 클래스
    template <typename T>
    concept ByteReadable = requires(T& object, ByteReader& reader) { object.Read(reader); };

    // 바이트를 값으로 읽는다(>>). ByteWriter가 쓴 순서대로 읽어야 한다. 형식은 ByteTraits.h 맨 위.
    //
    // 받은 버퍼를 복사하지 않고 가리키기만 한다(string_view와 같다).
    // 그래서 읽는 동안 원본 버퍼가 살아 있어야 한다. 받은 자리에서 읽고 끝내고, 다른 곳에 넘길 때는 읽은 객체를 넘긴다.
    //
    // 데이터가 모자라거나, 개수·인덱스가 맞지 않거나, UTF-8이 잘못되면 실패 표시를 남기고 이후 읽기는 모두 무시한다.
    // 실패한 값은 빈 값(T{}, 빈 컨테이너, nullopt)이 된다. 끝에서 IsValid()로 한 번 확인한다.
    // 읽는 타입은 기본 생성할 수 있어야 한다.
    class ByteReader
    {
    public:
        static constexpr std::size_t MaxCount{ByteMaxCount};

        explicit ByteReader(const std::span<const std::byte> data);

        // 판별 순서는 ByteWriter와 같다. 쓰기만 되는 타입(string_view, 리터럴, 포인터)은 읽을 수 없다.
        template <typename T>
        ByteReader& operator>>(T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                // 0/1이 아닌 바이트를 bool에 그대로 복사하면 미정의 동작이라 uint8_t로 읽어 바꾼다.
                auto raw = uint8_t{};
                *this >> raw;
                value = (raw != 0);
            }
            else if constexpr (Primitive<T>)
            {
                if (!ReadBytes(&value, sizeof(T)))
                {
                    value = T{};
                }
            }
            else if constexpr (IsByteString<T>)
            {
                ReadByteString(value);
            }
            else if constexpr (IsWideString<T>)
            {
                ReadWideString(value);
            }
            else if constexpr (IsByteStringView<T> || IsWideStringView<T> || CharArray<T> || CharPointer<T>)
            {
                static_assert(false, "문자열은 std::string(또는 wstring 등)으로 읽는다. view와 포인터는 받은 버퍼를 "
                                     "가리키게 되어 위험하다");
            }
            else if constexpr (IsOptional<T>)
            {
                ReadOptional(value);
            }
            else if constexpr (IsVariant<T>)
            {
                ReadVariant(value);
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
                // variant의 "빈 값". 읽을 내용이 없다.
            }
            else if constexpr (IsTupleLike<T>)
            {
                std::apply([this](auto&... elements) { ReadFields(elements...); }, value);
            }
            else if constexpr (IsDuration<T>)
            {
                auto count = typename T::rep{};
                *this >> count;
                value = T{count};
            }
            else if constexpr (IsTimePoint<T>)
            {
                auto count = typename T::rep{};
                *this >> count;
                value = T{typename T::duration{count}};
            }
            else if constexpr (ByteReadable<T>)
            {
                value.Read(*this);
            }
            else if constexpr (FixedArray<T>)
            {
                ReadFixedArray(value);
            }
            else if constexpr (KeyValueContainer<T>)
            {
                ReadKeyValues(value);
            }
            else if constexpr (ReadableSequence<T>)
            {
                ReadSequence(value);
            }
            else
            {
                static_assert(false, "ByteReader가 지원하지 않는 타입이다(포인터, 스마트 포인터, span 등)");
            }
            return *this;
        }

        // SERIALIZE_FIELDS가 부른다. 필드를 순서대로 읽는다(fold expression).
        template <typename... Ts>
        void ReadFields(Ts&... fields)
        {
            (*this >> ... >> fields);
        }

        std::size_t Size() const;
        std::size_t ReadPos() const;
        bool IsValid() const;

    private:
        bool ReadBytes(void* dst, const std::size_t size);
        // 길이·개수를 읽는다. 남은 바이트보다 크면 실패 표시 후 0.
        // 원소는 최소 1바이트이므로, 거짓 개수로 큰 메모리를 잡게 하는 패킷을 여기서 막는다.
        std::size_t ReadCount();

        // 길이 + 바이트를 그대로 읽는다(검사 없음).
        template <typename S>
        void ReadRawString(S& value)
        {
            value.clear();
            const auto length = ReadCount(); // 남은 바이트 이하가 보장된다
            value.resize(length);
            if (!ReadBytes(value.data(), length))
            {
                value.clear();
            }
        }

        // std::string, std::u8string: 올바른 UTF-8이 아니면 실패한다(필터 우회, DB·클라이언트 깨짐 방지).
        // 텍스트가 아닌 바이너리 데이터는 string이 아니라 vector<std::byte>에 담는다.
        template <typename S>
        void ReadByteString(S& value)
        {
            ReadRawString(value);
            const auto text = std::string_view{reinterpret_cast<const char*>(value.data()), value.size()};
            if (_valid && !IsValidUtf8(text))
            {
                _valid = false;
                value.clear();
            }
        }

        // UTF-8로 읽어 wstring/u16string/u32string으로 바꾼다. 잘못된 UTF-8이면 실패(검사는 DecodeUtf8이 한다).
        template <typename S>
        void ReadWideString(S& value)
        {
            auto utf8 = std::string{};
            ReadRawString(utf8);
            if (!_valid || !DecodeUtf8(utf8, value))
            {
                _valid = false;
                value.clear();
            }
        }

        template <typename O>
        void ReadOptional(O& value)
        {
            auto hasValue = false;
            *this >> hasValue;
            if (!hasValue)
            {
                value.reset();
                return;
            }
            auto inner = typename O::value_type{};
            *this >> inner;
            if (_valid)
            {
                value = std::move(inner);
            }
            else
            {
                value.reset();
            }
        }

        template <typename V>
        void ReadVariant(V& value)
        {
            auto index = uint8_t{};
            *this >> index;
            if (_valid && index >= std::variant_size_v<V>)
            {
                _valid = false; // 없는 대안 번호
            }
            if (!_valid)
            {
                value = V{};
                return;
            }
            ReadVariantAt(value, index, std::make_index_sequence<std::variant_size_v<V>>{});
        }

        // 실행 중에 정해지는 index를 컴파일 시간 대안 번호 I로 바꿔 그 타입으로 읽는다.
        template <typename V, std::size_t... I>
        void ReadVariantAt(V& value, const std::size_t index, std::index_sequence<I...>)
        {
            static_cast<void>(((index == I ? (ReadAlternative<I>(value), true) : false) || ...));
        }

        template <std::size_t I, typename V>
        void ReadAlternative(V& value)
        {
            auto alternative = std::variant_alternative_t<I, V>{};
            *this >> alternative;
            if (_valid)
            {
                value.template emplace<I>(std::move(alternative));
            }
            else
            {
                value = V{};
            }
        }

        // 개수를 읽어 배열 크기와 같은지 확인한다. 다르면 실패.
        template <typename A>
        void ReadFixedArray(A& values)
        {
            static_assert(!IsZeroSize<std::ranges::range_value_t<A>>,
                          "크기가 0인 원소(monostate, 빈 tuple)의 배열은 지원하지 않는다");
            const auto count = ReadCount();
            if (_valid && count != std::size(values))
            {
                _valid = false;
            }
            for (auto& value : values)
            {
                *this >> value;
            }
            if (!_valid)
            {
                ResetValue(values);
            }
        }

        // 빈 값으로 되돌린다. C 배열은 대입이 안 되므로 원소마다 재귀로(다차원 배열 int[2][2] 포함).
        template <typename T>
        static void ResetValue(T& value)
        {
            if constexpr (std::is_bounded_array_v<T>)
            {
                for (auto& element : value)
                {
                    ResetValue(element);
                }
            }
            else
            {
                value = T{};
            }
        }

        template <typename C>
        void ReadSequence(C& values)
        {
            static_assert(!IsZeroSize<typename C::value_type>,
                          "크기가 0인 원소(monostate, 빈 tuple)의 컨테이너는 지원하지 않는다");
            values.clear();
            const auto count = ReadCount();
            if constexpr (requires { values.reserve(count); })
            {
                values.reserve(count);
            }
            for (auto i = std::size_t{}; i < count && _valid; ++i)
            {
                auto value = typename C::value_type{};
                *this >> value;
                if constexpr (requires { values.emplace_back(std::move(value)); })
                {
                    values.emplace_back(std::move(value)); // vector, deque, list
                }
                else
                {
                    values.insert(std::move(value)); // set, unordered_set, multiset
                }
            }
            if (!_valid)
            {
                values.clear();
            }
        }

        template <typename M>
        void ReadKeyValues(M& values)
        {
            static_assert(!(IsZeroSize<typename M::key_type> && IsZeroSize<typename M::mapped_type>),
                          "키와 값이 모두 크기 0인 컨테이너는 지원하지 않는다");
            values.clear();
            const auto count = ReadCount();
            for (auto i = std::size_t{}; i < count && _valid; ++i)
            {
                auto key = typename M::key_type{};
                auto value = typename M::mapped_type{};
                *this >> key >> value;
                // multimap도 되도록 emplace(중복 키: map은 먼저 온 값 유지)
                values.emplace(std::move(key), std::move(value));
            }
            if (!_valid)
            {
                values.clear();
            }
        }

        std::span<const std::byte> _data{};
        std::size_t _readPos{};
        bool _valid{true};
    };
} // namespace Base
