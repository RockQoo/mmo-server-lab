#include "Base/ByteReader.h"

#include <cstring>

namespace Base
{
    ByteReader::ByteReader(const std::span<const std::byte> data) : _data{data}
    {
    }

    std::size_t ByteReader::Size() const
    {
        return _data.size();
    }

    std::size_t ByteReader::ReadPos() const
    {
        return _readPos;
    }

    bool ByteReader::IsValid() const
    {
        return _valid;
    }

    bool ByteReader::ReadBytes(void* dst, const std::size_t size)
    {
        if (!_valid || _data.size() - _readPos < size)
        {
            _valid = false;
            return false;
        }
        std::memcpy(dst, _data.data() + _readPos, size);
        _readPos += size;
        return true;
    }

    std::size_t ByteReader::ReadCount()
    {
        auto count = uint16_t{};
        *this >> count;
        if (count > _data.size() - _readPos)
        {
            _valid = false;
            return 0;
        }
        return count;
    }
} // namespace Base
