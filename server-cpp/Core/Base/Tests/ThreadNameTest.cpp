#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "Base/ThreadName.h"

// 붙인 이름을 OS 에서 다시 읽어 확인한다(테스트만 OS 함수를 직접 부른다)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
    std::wstring CurrentThreadName()
    {
        PWSTR description = nullptr;
        std::wstring name;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)))
        {
            name = description;
            LocalFree(description);
        }
        return name;
    }
} // namespace

TEST(ThreadName, SetsUtf8NameOnCurrentThread)
{
    std::wstring seen;
    auto ok = false;
    std::thread worker(
        [&]
        {
            ok = Base::SetCurrentThreadName("Zone-3 존");
            seen = CurrentThreadName();
        });
    worker.join();

    EXPECT_TRUE(ok);
    EXPECT_EQ(seen, L"Zone-3 존");
}

TEST(ThreadName, EmptyNameIsSkippedAndBrokenUtf8Fails)
{
    std::wstring seen;
    auto empty = false;
    auto broken = true;
    std::thread worker(
        [&]
        {
            empty = Base::SetCurrentThreadName("");
            broken = Base::SetCurrentThreadName("\xFF");
            seen = CurrentThreadName();
        });
    worker.join();

    EXPECT_TRUE(empty);
    EXPECT_FALSE(broken);
    EXPECT_EQ(seen, L""); // 둘 다 이름을 붙이지 않았다
}
