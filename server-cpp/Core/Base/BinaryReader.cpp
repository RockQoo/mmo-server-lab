#include "Base/BinaryReader.h"

#include <cstring>

namespace Base
{
    BinaryReader::BinaryReader(const std::span<const std::byte> data) : _data{data}
    {
    }

    std::size_t BinaryReader::Size() const
    {
        return _data.size();
    }

    std::size_t BinaryReader::ReadPos() const
    {
        return _readPos;
    }

    bool BinaryReader::IsValid() const
    {
        return _valid;
    }

    bool BinaryReader::ReadBytes(void* dst, const std::size_t size)
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

    std::size_t BinaryReader::ReadCount()
    {
        auto count = std::uint16_t{};
        *this >> count;
        if (count > _data.size() - _readPos)
        {
            _valid = false;
            return 0;
        }
        return count;
    }

    void BinaryReader::ReadString(std::string& value)
    {
        value.clear();
        const auto length = ReadCount(); // 남은 바이트 이하가 보장된다
        value.resize(length);
        if (!ReadBytes(value.data(), length))
        {
            value.clear();
        }
    }
} // namespace Base
