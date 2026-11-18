#include <gtest/gtest.h>

#include "Base/BuildInfo.h"

// 벤치마크 기록에 남길 측정 조건이 빠짐없이 들어 있는지 확인한다.
TEST(BuildInfo, ContainsCompilerStandardAndConfig)
{
    const std::string info = Base::BuildInfo();

    EXPECT_NE(info.find("MSVC "), std::string::npos) << info;
    EXPECT_NE(info.find("__cplusplus="), std::string::npos) << info;

#if defined(NDEBUG)
    EXPECT_NE(info.find("Release"), std::string::npos) << info;
#else
    EXPECT_NE(info.find("Debug"), std::string::npos) << info;
#endif
}

// /Zc:__cplusplus가 빠지면 MSVC는 199711L을 보고한다. C++23 이상인지 확인한다.
TEST(BuildInfo, CplusplusIsAtLeastCpp23)
{
    EXPECT_GE(__cplusplus, 202302L);
}
