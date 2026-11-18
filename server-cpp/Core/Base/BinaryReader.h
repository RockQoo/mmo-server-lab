#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Base/BinaryTraits.h"

namespace Base
{
    class BinaryReader;

    // SERIALIZE_FIELDS로 Read(BinaryReader&)를 가진 클래스
    template <typename T>
    concept BinaryReadable = requires(T& object, BinaryReader& reader) { object.Read(reader); };

    // 바이트를 값으로 읽는다(>>). BinaryWriter가 쓴 순서대로 읽어야 한다.
    //
    // 받은 버퍼를 복사하지 않고 가리키기만 한다(string_view와 같다).
    // 그래서 읽는 동안 원본 버퍼가 살아 있어야 한다. 받은 자리에서 읽고 끝내고, 다른 곳에 넘길 때는 읽은 객체를 넘긴다.
    //
    // 데이터가 모자라거나 개수가 맞지 않으면 실패 표시를 남기고 이후 읽기는 모두 무시한다.
    // 실패한 값은 빈 값(T{}, 빈 컨테이너)이 된다. 끝에서 IsValid()로 한 번 확인한다.
    class BinaryReader
    {
    public:
        static constexpr std::size_t MaxCount{BinaryMaxCount};

        explicit BinaryReader(const std::span<const std::byte> data);

        // 판별 순서는 BinaryWriter와 같다.
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
            else if constexpr (std::is_same_v<T, std::string>)
            {
                ReadString(value);
            }
            else if constexpr (IsVector<T>)
            {
                ReadSequence(value);
            }
            else if constexpr (KeyValueContainer<T>)
            {
                ReadKeyValues(value);
            }
            else if constexpr (BinaryReadable<T>)
            {
                value.Read(*this);
            }
            else
            {
                static_assert(false, "BinaryReader가 지원하지 않는 타입이다");
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
        void ReadString(std::string& value);

        template <typename C>
        void ReadSequence(C& values)
        {
            values.clear();
            const auto count = ReadCount();
            values.reserve(count);
            for (auto i = std::size_t{}; i < count && _valid; ++i)
            {
                auto value = typename C::value_type{};
                *this >> value;
                values.push_back(std::move(value));
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
                values.insert_or_assign(std::move(key), std::move(value));
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
