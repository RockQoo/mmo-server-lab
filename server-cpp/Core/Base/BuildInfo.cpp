#include "Base/BuildInfo.h"

#include <format>

namespace Base
{
    std::string BuildInfo()
    {
#if defined(_MSC_FULL_VER)
        // 예: 195136260 → 19.51.36260
        constexpr auto FullVersion = _MSC_FULL_VER;
        const auto compiler =
            std::format("MSVC {}.{}.{}", FullVersion / 10000000, FullVersion / 100000 % 100, FullVersion % 100000);
#else
        const auto compiler = std::string{"unknown"};
#endif

#if defined(NDEBUG)
        constexpr auto Config = "Release";
#else
        constexpr auto Config = "Debug";
#endif

        return std::format("{}, __cplusplus={}, {}", compiler, __cplusplus, Config);
    }
} // namespace Base
