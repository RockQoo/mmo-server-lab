#include "Base/Stream.h"

#include <cstdint>
#include <cstring>

namespace Base
{
    Stream::Stream(const std::span<const std::byte> data) : _buffer(data.begin(), data.end())
    {
    }

    // bool은 1바이트(0 또는 1)로 쓴다. 0/1이 아닌 바이트를 bool에 그대로 복사하면 미정의 동작이라
    // uint8_t로 읽고 0이 아니면 true로 바꾼다.
    Stream& Stream::operator<<(const bool value)
    {
        return *this << static_cast<std::uint8_t>(value ? 1 : 0);
    }

    Stream& Stream::operator>>(bool& value)
    {
        auto raw = std::uint8_t{};
        *this >> raw;
        value = (raw != 0);
        return *this;
    }

    std::span<const std::byte> Stream::Data() const
    {
        return _buffer;
    }

    std::size_t Stream::Size() const
    {
        return _buffer.size();
    }

    std::size_t Stream::Capacity() const
    {
        return _buffer.capacity();
    }

    std::size_t Stream::ReadPos() const
    {
        return _readPos;
    }

    bool Stream::IsValid() const
    {
        return _valid;
    }

    void Stream::Clear()
    {
        _buffer.clear(); // vector::clear는 capacity를 줄이지 않는다
        _readPos = 0;
        _valid = true;
    }

    void Stream::WriteBytes(const void* src, const std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(src);
        _buffer.insert(_buffer.end(), bytes, bytes + size);
    }

    bool Stream::ReadBytes(void* dst, const std::size_t size)
    {
        if (!_valid || _buffer.size() - _readPos < size)
        {
            _valid = false;
            return false;
        }
        std::memcpy(dst, _buffer.data() + _readPos, size);
        _readPos += size;
        return true;
    }
} // namespace Base
