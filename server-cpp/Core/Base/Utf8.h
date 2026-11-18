#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// UTF-8 ↔ UTF-16/UTF-32 변환. 직렬화 형식은 문자열을 항상 UTF-8로 쓰므로,
// wstring(Windows는 UTF-16, Linux는 UTF-32), u16string, u32string을 쓰고 읽을 때 여기서 바꾼다.
// 잘못된 입력(짝이 안 맞는 서로게이트, 범위 밖 코드 포인트, overlong, 잘린 바이트)은 고치지 않고 실패로 돌려준다.
namespace Base
{
    // 코드 포인트 하나를 UTF-8로 붙인다. 서로게이트 범위나 0x10FFFF를 넘으면 false.
    bool AppendUtf8(const char32_t codePoint, std::string& out);

    // in[pos]부터 UTF-8 코드 포인트 하나를 읽고 pos를 넘긴다. 잘못된 바이트면 false(pos는 그대로).
    bool DecodeCodePoint(const std::string_view in, std::size_t& pos, char32_t& codePoint);

    // UTF-16(2바이트 문자) 또는 UTF-32(4바이트 문자) → UTF-8
    template <typename C>
    bool EncodeUtf8(const std::basic_string_view<C> in, std::string& out)
    {
        static_assert(sizeof(C) == 2 || sizeof(C) == 4, "UTF-16 또는 UTF-32 문자만 지원한다");
        out.clear();
        for (auto i = std::size_t{}; i < in.size(); ++i)
        {
            auto codePoint = static_cast<char32_t>(in[i]);
            if constexpr (sizeof(C) == 2)
            {
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) // 앞 서로게이트: 뒤 서로게이트와 짝을 이뤄야 한다
                {
                    if (i + 1 >= in.size())
                    {
                        return false;
                    }
                    const auto low = static_cast<char32_t>(in[i + 1]);
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        return false;
                    }
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            if (!AppendUtf8(codePoint, out)) // 홀로 남은 뒤 서로게이트도 여기서 걸린다
            {
                return false;
            }
        }
        return true;
    }

    // UTF-8 → UTF-16(2바이트 문자) 또는 UTF-32(4바이트 문자)
    template <typename C, typename Traits, typename Alloc>
    bool DecodeUtf8(const std::string_view in, std::basic_string<C, Traits, Alloc>& out)
    {
        static_assert(sizeof(C) == 2 || sizeof(C) == 4, "UTF-16 또는 UTF-32 문자만 지원한다");
        out.clear();
        auto pos = std::size_t{};
        while (pos < in.size())
        {
            auto codePoint = char32_t{};
            if (!DecodeCodePoint(in, pos, codePoint))
            {
                return false;
            }
            if constexpr (sizeof(C) == 2)
            {
                if (codePoint >= 0x10000) // BMP 밖은 서로게이트 쌍으로
                {
                    const auto offset = codePoint - 0x10000;
                    out.push_back(static_cast<C>(0xD800 + (offset >> 10)));
                    out.push_back(static_cast<C>(0xDC00 + (offset & 0x3FF)));
                    continue;
                }
            }
            out.push_back(static_cast<C>(codePoint));
        }
        return true;
    }
} // namespace Base
