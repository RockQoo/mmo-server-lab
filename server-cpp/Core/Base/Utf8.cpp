#include "Base/Utf8.h"

namespace Base
{
    namespace
    {
        constexpr char32_t MaxCodePoint{0x10FFFF};

        bool IsSurrogate(const char32_t codePoint)
        {
            return codePoint >= 0xD800 && codePoint <= 0xDFFF;
        }
    } // namespace

    bool AppendUtf8(const char32_t codePoint, std::string& out)
    {
        if (codePoint > MaxCodePoint || IsSurrogate(codePoint))
        {
            return false;
        }

        if (codePoint < 0x80)
        {
            out.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        return true;
    }

    bool DecodeCodePoint(const std::string_view in, std::size_t& pos, char32_t& codePoint)
    {
        const auto lead = static_cast<unsigned char>(in[pos]);
        if (lead < 0x80)
        {
            codePoint = lead;
            ++pos;
            return true;
        }

        // 첫 바이트로 길이를 정한다. 각 길이로 표현해야 하는 최솟값보다 작으면 overlong(같은 문자를 길게 쓴 것)이라
        // 거부한다.
        auto length = std::size_t{};
        auto minimum = char32_t{};
        if ((lead & 0xE0) == 0xC0)
        {
            length = 2;
            codePoint = lead & 0x1F;
            minimum = 0x80;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            length = 3;
            codePoint = lead & 0x0F;
            minimum = 0x800;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            length = 4;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }

        if (in.size() - pos < length)
        {
            return false;
        }
        for (auto i = std::size_t{1}; i < length; ++i)
        {
            const auto next = static_cast<unsigned char>(in[pos + i]);
            if ((next & 0xC0) != 0x80)
            {
                return false;
            }
            codePoint = (codePoint << 6) | (next & 0x3F);
        }

        if (codePoint < minimum || codePoint > MaxCodePoint || IsSurrogate(codePoint))
        {
            return false;
        }
        pos += length;
        return true;
    }
} // namespace Base
