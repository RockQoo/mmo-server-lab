#include <gtest/gtest.h>

#include "Base/Assert.h"

// 죽는 것을 확인하는 테스트(death test)는 이름 끝을 DeathTest로 한다. gtest가 먼저 돌린다.
// EXPECT_DEATH는 자식 프로세스에서 문장을 실행하고, 프로세스가 죽었는지와 stderr 출력(정규식)을 본다.

TEST(Assert, PassesWhenTrue)
{
    ASSERT(1 + 1 == 2, "never fails");
    SUCCEED();
}

TEST(Assert, EvaluatesConditionOnce)
{
    auto calls = 0;
    ASSERT(++calls == 1, "condition must run once");
    EXPECT_EQ(calls, 1);
}

TEST(AssertDeathTest, CrashesWhenFalse)
{
    EXPECT_DEATH(ASSERT(1 + 1 == 3, "math broke"), "math broke");
}

TEST(AssertDeathTest, ReportsConditionText)
{
    EXPECT_DEATH(ASSERT(1 + 1 == 3, "x"), "1 \\+ 1 == 3");
}

// abort 같은 다른 이유가 아니라 우리 예외(RaiseException)로 죽는다. 종료 코드 = 예외 코드.
// gtest는 기본으로 테스트 안의 SEH 예외를 잡아 종료 코드 1로 바꾼다. 그래서 --gtest_catch_exceptions=0일 때만 확인한다
// (ctest는 CMakeLists.txt에서 자동으로 붙인다. BaseTests.exe를 그냥 실행하면 이 테스트는 건너뛴다).
TEST(AssertDeathTest, ExitsWithAssertExceptionCode)
{
    if (GTEST_FLAG_GET(catch_exceptions))
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto isAssertCrash = [](const int exitCode) {
        return static_cast<unsigned long>(exitCode) == Base::AssertExceptionCode;
    };
    EXPECT_EXIT(ASSERT(false, "code check"), isAssertCrash, "code check");
}

TEST(AssertDeathTest, DebugAssertOnlyInDebug)
{
#ifdef NDEBUG
    auto calls = 0;
    DEBUG_ASSERT(++calls == 0, "not evaluated in release");
    EXPECT_EQ(calls, 0);
#else
    EXPECT_DEATH(DEBUG_ASSERT(false, "debug only"), "debug only");
#endif
}
