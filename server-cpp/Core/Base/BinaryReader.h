#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Base/BinaryTraits.h"
#include "Base/Utf8.h"

namespace Base
{
    class BinaryReader;

    // SERIALIZE_FIELDS로 Read(BinaryReader&)를 가진 클래스
    template <typename T>
    concept BinaryReadable = requires(T& object, BinaryReader& reader) { object.Read(reader); };

    // 바이트를 값으로 읽는다(>>). BinaryWriter가 쓴 순서대로 읽어야 한다. 형식은 BinaryTraits.h 맨 위.
    //
    // 받은 버퍼를 복사하지 않고 가리키기만 한다(string_view와 같다).
    // 그래서 읽는 동안 원본 버퍼가 살아 있어야 한다. 받은 자리에서 읽고 끝내고, 다른 곳에 넘길 때는 읽은 객체를 넘긴다.
    //
    // 데이터가 모자라거나, 개수·인덱스가 맞지 않거나, UTF-8이 잘못되면 실패 표시를 남기고 이후 읽기는 모두 무시한다.
    // 실패한 값은 빈 값(T{}, 빈 컨테이너, nullopt)이 된다. 끝에서 IsValid()로 한 번 확인한다.
    // 읽는 타입은 기본 생성할 수 있어야 한다.
    class BinaryReader
    {
    public:
        static constexpr std::size_t MaxCount{BinaryMaxCount};

        explicit BinaryReader(const std::span<const std::byte> data);

        // 판별 순서는 BinaryWriter와 같다. 쓰기만 되는 타입(string_view, 리터럴, 포인터)은 읽을 수 없다.
        template <typename T>
        BinaryReader& operator>>(T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                // 0/1이 아닌 바이트를 bool에 그대로 복사하면 미정의 동작이라 uint8_t로 읽어 바꾼다.
                auto raw = std::uint8_t{};
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
            else if constexpr (ByteString<T>)
            {
                ReadByteString(value);
            }
            else if constexpr (IsWideString<T>)
            {
                ReadWideString(value);
            }
            else if constexpr (ByteStringView<T> || IsWideStringView<T> || CharArray<T> || CharPointer<T>)
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
            else if constexpr (BinaryReadable<T>)
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
                static_assert(false, "BinaryReader가 지원하지 않는 타입이다(포인터, 스마트 포인터, span 등)");
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

        // std::string, std::u8string: 길이 + 바이트
        template <typename S>
        void ReadByteString(S& value)
        {
            value.clear();
            const auto length = ReadCount(); // 남은 바이트 이하가 보장된다
            value.resize(length);
            if (!ReadBytes(value.data(), length))
            {
                value.clear();
            }
        }

        // UTF-8로 읽어 wstring/u16string/u32string으로 바꾼다. 잘못된 UTF-8이면 실패.
        template <typename S>
        void ReadWideString(S& value)
        {
            auto utf8 = std::string{};
            ReadByteString(utf8);
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
            auto index = std::uint8_t{};
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
                for (auto& value : values)
                {
                    value = std::remove_cvref_t<decltype(value)>{};
                }
            }
        }

        template <typename C>
        void ReadSequence(C& values)
        {
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
