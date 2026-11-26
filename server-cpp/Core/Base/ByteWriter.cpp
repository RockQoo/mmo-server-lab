#include "Base/ByteWriter.h"

namespace Base
{
    std::span<const std::byte> ByteWriter::Data() const
    {
        return _buffer;
    }

    std::size_t ByteWriter::Size() const
    {
        return _buffer.size();
    }

    std::size_t ByteWriter::Capacity() const
    {
        return _buffer.capacity();
    }

    bool ByteWriter::IsValid() const
    {
        return _valid;
    }

    void ByteWriter::Clear()
    {
        _buffer.clear(); // vector::clear는 capacity를 줄이지 않는다
        _valid = true;
    }

    void ByteWriter::WriteBytes(const void* src, const std::size_t size)
    {
        if (!_valid)
        {
            return;
        }
        const auto* bytes = static_cast<const std::byte*>(src);
        _buffer.insert(_buffer.end(), bytes, bytes + size);
    }

    bool ByteWriter::WriteCount(const std::size_t count)
    {
        if (count > MaxCount)
        {
            _valid = false;
            return false;
        }
        *this << static_cast<uint16_t>(count);
        return _valid;
    }

    void ByteWriter::WriteString(const void* data, const std::size_t size)
    {
        if (WriteCount(size))
        {
            WriteBytes(data, size);
        }
    }

    void ByteWriter::WriteCString(const char* value)
    {
        if (!_valid || value == nullptr)
        {
            _valid = false;
            return;
        }
        // 널 문자를 MaxCount + 1바이트까지만 찾는다. 그보다 길면 어차피 쓸 수 없으므로 끝까지 훑지 않는다.
        // (올바른 C 문자열은 널 문자 앞에서 멈추므로 버퍼 밖을 읽지 않는다)
        auto length = std::size_t{};
        while (length <= MaxCount && value[length] != '\0')
        {
            ++length;
        }
        WriteString(value, length); // length > MaxCount면 WriteCount가 실패 처리
    }
} // namespace Base
