#include "Core/Util/BuildInfo.h"

#include <format>

namespace Core
{
    std::string BuildInfo()
    {
#if defined(_MSC_FULL_VER)
        // 예: 195136260 → 19.51.36260
        constexpr int ver = _MSC_FULL_VER;
        const std::string compiler = std::format("MSVC {}.{}.{}", ver / 10000000, ver / 100000 % 100, ver % 100000);
#else
        const std::string compiler = "unknown";
#endif

#if defined(NDEBUG)
        constexpr const char* config = "Release";
#else
        constexpr const char* config = "Debug";
#endif

        return std::format("{}, __cplusplus={}, {}", compiler, __cplusplus, config);
    }
} // namespace Core
