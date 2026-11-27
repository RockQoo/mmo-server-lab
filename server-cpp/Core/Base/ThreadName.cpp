#include "Base/ThreadName.h"

#include <string>

// windows.h 의 min/max 매크로와 잘 안 쓰는 부분은 끈다
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Base/Utf8.h"

namespace Base
{
    bool SetCurrentThreadName(const std::string_view name)
    {
        if (name.empty())
        {
            return true;
        }
        std::wstring wide; // Windows API 가 UTF-16 을 받는다
        if (!DecodeUtf8(name, wide))
        {
            return false;
        }
        return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wide.c_str()));
    }
} // namespace Base
