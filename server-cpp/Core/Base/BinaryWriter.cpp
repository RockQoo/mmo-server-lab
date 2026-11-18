#include "Base/BinaryWriter.h"

namespace Base
{
    std::span<const std::byte> BinaryWriter::Data() const
    {
        return _buffer;
    }

    std::size_t BinaryWriter::Size() const
    {
        return _buffer.size();
    }

    std::size_t BinaryWriter::Capacity() const
    {
        return _buffer.capacity();
    }

    bool BinaryWriter::IsValid() const
    {
        return _valid;
    }

    void BinaryWriter::Clear()
    {
        _buffer.clear(); // vector::clear는 capacity를 줄이지 않는다
        _valid = true;
    }

    void BinaryWriter::WriteBytes(const void* src, const std::size_t size)
    {
        if (!_valid)
        {
            return;
        }
        const auto* bytes = static_cast<const std::byte*>(src);
        _buffer.insert(_buffer.end(), bytes, bytes + size);
    }

    bool BinaryWriter::WriteCount(const std::size_t count)
    {
        if (count > MaxCount)
        {
            _valid = false;
            return false;
        }
        *this << static_cast<std::uint16_t>(count);
        return _valid;
    }

    void BinaryWriter::WriteString(const std::string& value)
    {
        if (WriteCount(value.size()))
        {
            WriteBytes(value.data(), value.size());
        }
    }
} // namespace Base
