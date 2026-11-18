#pragma once

#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace Base
{
    // 직렬화 형식은 리틀 엔디안 고정. x86/x64는 원래 리틀 엔디안이라 변환 없이 그대로 복사한다.
    static_assert(std::endian::native == std::endian::little, "Stream은 리틀 엔디안 CPU만 지원한다");

    // 메모리를 그대로 복사해도 되는 기본형: 정수, 실수, enum. bool은 따로 처리한다.
    template <typename T>
    concept Primitive = (std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<T, bool>;

    // 값을 바이트로 쓰고(<<), 같은 순서로 다시 읽는다(>>).
    // 읽다가 데이터가 모자라면 실패 표시를 남기고 이후 읽기는 모두 무시한다. 끝에서 IsValid()로 한 번 확인한다.
    class Stream
    {
    public:
        Stream() = default;
        explicit Stream(const std::span<const std::byte> data); // 받은 데이터를 읽기용으로 담는다

        template <Primitive T>
        Stream& operator<<(const T value)
        {
            WriteBytes(&value, sizeof(T));
            return *this;
        }

        template <Primitive T>
        Stream& operator>>(T& value)
        {
            if (!ReadBytes(&value, sizeof(T)))
            {
                value = T{};
            }
            return *this;
        }

        Stream& operator<<(const bool value);
        Stream& operator>>(bool& value);

        std::span<const std::byte> Data() const;
        std::size_t Size() const;
        std::size_t Capacity() const;
        std::size_t ReadPos() const;
        bool IsValid() const;

        // 내용과 읽기 위치를 비우되 확보한 메모리(capacity)는 유지한다. 재사용과 나중의 풀링을 위한 것.
        void Clear();

    private:
        void WriteBytes(const void* src, const std::size_t size);
        bool ReadBytes(void* dst, const std::size_t size);

        std::vector<std::byte> _buffer{};
        std::size_t _readPos{};
        bool _valid{true};
    };
} // namespace Base
