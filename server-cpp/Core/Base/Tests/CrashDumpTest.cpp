#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Base/CrashDump.h"

// 크래시 덤프는 자식 프로세스(death test)에서 일부러 크래시 내고, 부모가 덤프 파일이 생겼는지 본다.
// Install 은 프로세스마다 한 번이라 InstallOnce 로 부른다(BaseTests.exe 를 그냥 돌리면 한 프로세스에서 모두 돈다).
namespace
{
    std::filesystem::path TestDir(const std::string_view name)
    {
        return std::filesystem::temp_directory_path() / "mmo-server-lab-CrashDump" / name;
    }

    void InstallOnce(const std::filesystem::path& directory)
    {
        static const auto Installed = [&directory]
        {
            Base::CrashDump::Install({.directory = directory});
            return true;
        }();
        static_cast<void>(Installed);
    }

    // directory 에 이름이 suffix 로 끝나는 .dmp 가 있고 비어 있지 않은가
    bool HasDump(const std::filesystem::path& directory, const std::wstring_view suffix)
    {
        if (!std::filesystem::exists(directory))
        {
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator{directory})
        {
            if (entry.path().filename().wstring().ends_with(suffix) && entry.file_size() > 0)
            {
                return true;
            }
        }
        return false;
    }

    // 크래시 덤프는 처리 안 된 예외가 OS 까지 가야 남는다. gtest 가 SEH 예외를 잡으면(기본) 확인할 수 없다.
    bool CanSeeCrashes()
    {
        return !GTEST_FLAG_GET(catch_exceptions);
    }
} // namespace

// 수동 덤프는 다른 테스트와 같은 프로세스에서 돌 수 있어 모든 테스트가 같은 폴더로 Install 한다.
TEST(CrashDump, WritesManualDumpAndKeepsRunning)
{
    const auto dir = TestDir("shared");
    InstallOnce(dir);

    const auto path = Base::CrashDump::Write(Base::DumpKind::Mini, "manual test");

    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(path.filename().wstring().ends_with(L"_manual_test.dmp")) << path; // 공백은 _
    EXPECT_GT(std::filesystem::file_size(path), 0U);
    std::filesystem::remove(path);
}

TEST(CrashDumpDeathTest, WritesDumpOnUnhandledException)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("shared");
    std::filesystem::remove_all(dir);
    EXPECT_EXIT(
        {
            InstallOnce(dir);
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        },
        [](int) { return true; }, "");
    EXPECT_TRUE(HasDump(dir, L"_crash.dmp"));
}

// 다른 스레드에서 terminate(예외가 스레드 밖으로 나감): MSVC 의 terminate 처리기는 스레드마다라 SIGABRT 로 잡는다
TEST(CrashDumpDeathTest, WritesDumpOnTerminateInOtherThread)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("shared");
    std::filesystem::remove_all(dir);
    EXPECT_EXIT(
        {
            InstallOnce(dir);
            std::thread{[] { std::terminate(); }}.join();
        },
        [](int) { return true; }, "");
    EXPECT_TRUE(HasDump(dir, L"_abort.dmp"));
}
