#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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

    void InstallOnce(const Base::CrashDumpOptions& options)
    {
        static const auto Installed = [&options]
        {
            Base::CrashDump::Install(options);
            return true;
        }();
        static_cast<void>(Installed);
    }

    void InstallOnce(const std::filesystem::path& directory)
    {
        InstallOnce(Base::CrashDumpOptions{.directory = directory});
    }

    // afterCrash 는 함수 포인터라 캡처가 없다. 불렸다는 표시를 이 파일에 남긴다.
    std::filesystem::path AfterCrashMarker()
    {
        return TestDir("aftercrash") / "aftercrash.txt";
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

// Full 덤프를 스냅샷(기본)과 바로 쓰기 두 방법 모두로 남긴다. 둘 다 프로세스 메모리 전체라 Mini 보다 크다.
TEST(CrashDump, WritesFullDumpBySnapshotAndDirect)
{
    const auto dir = TestDir("shared");
    InstallOnce(dir);

    const auto mini = Base::CrashDump::Write(Base::DumpKind::Mini, "mini");
    const auto snapshot = Base::CrashDump::Write(Base::DumpKind::Full, "snapshot");
    const auto direct = Base::CrashDump::Write(Base::DumpKind::Full, "direct", Base::DumpMethod::Direct);

    ASSERT_FALSE(mini.empty());
    ASSERT_FALSE(snapshot.empty());
    ASSERT_FALSE(direct.empty());
    EXPECT_GT(std::filesystem::file_size(snapshot), std::filesystem::file_size(mini));
    EXPECT_GT(std::filesystem::file_size(direct), std::filesystem::file_size(mini));
    std::filesystem::remove(mini);
    std::filesystem::remove(snapshot);
    std::filesystem::remove(direct);
}

TEST(CrashDumpDeathTest, WritesDumpOnUnhandledException)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("unhandled");
    std::filesystem::remove_all(dir);
    EXPECT_EXIT(
        {
            InstallOnce(dir);
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        },
        [](int32_t) { return true; }, "");
    EXPECT_TRUE(HasDump(dir, L"_crash.dmp"));
}

// 다른 스레드에서 terminate(예외가 스레드 밖으로 나감): MSVC 의 terminate 처리기는 스레드마다라 SIGABRT 로 잡는다
TEST(CrashDumpDeathTest, WritesDumpOnTerminateInOtherThread)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("terminate");
    std::filesystem::remove_all(dir);
    EXPECT_EXIT(
        {
            InstallOnce(dir);
            std::thread{[] { std::terminate(); }}.join();
        },
        [](int32_t) { return true; }, "");
    EXPECT_TRUE(HasDump(dir, L"_abort.dmp"));
}

// 덤프 뒤 afterCrash(서버는 Log::Flush)를 부르고 나서 끝난다
TEST(CrashDumpDeathTest, CallsAfterCrashBeforeExit)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("aftercrash");
    std::filesystem::remove_all(dir);
    EXPECT_EXIT(
        {
            InstallOnce({.directory = dir, .afterCrash = [] { std::ofstream{AfterCrashMarker()} << "called"; }});
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        },
        [](int32_t) { return true; }, "");
    EXPECT_TRUE(HasDump(dir, L"_crash.dmp"));
    EXPECT_TRUE(std::filesystem::exists(AfterCrashMarker()));
}

// afterCrash 가 끝나지 않아도(크래시 난 스레드가 로그 Lane 인 경우) 시간 제한 뒤 끝난다. 못 끝나면 EXPECT_EXIT 가
// 돌아오지 않아 테스트가 시간 초과로 실패한다.
TEST(CrashDumpDeathTest, GivesUpAfterCrashAfterTimeout)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("aftercrash-timeout");
    std::filesystem::remove_all(dir);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EXIT(
        {
            InstallOnce({.directory = dir,
                         .afterCrash = [] { Sleep(INFINITE); },
                         .afterCrashTimeout = std::chrono::milliseconds{200}});
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        },
        [](int32_t) { return true; }, "");
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{10});
    EXPECT_TRUE(HasDump(dir, L"_crash.dmp"));
}

// 같은 초에 같은 이유로 연달아 남겨도 덮어쓰지 않는다(Codex 리뷰 R4)
TEST(CrashDump, KeepsBothDumpsWithSameNameInSameSecond)
{
    const auto dir = TestDir("shared");
    InstallOnce(dir);

    const auto first = Base::CrashDump::Write(Base::DumpKind::Mini, "same");
    const auto second = Base::CrashDump::Write(Base::DumpKind::Mini, "same");

    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    EXPECT_TRUE(std::filesystem::exists(first));
    EXPECT_TRUE(std::filesystem::exists(second));
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

// afterCrash 안에서 덤프 스레드 자신이 크래시 나도 기다리지 않고 끝난다(Codex 리뷰 R2). 덤프 스레드가 다시 덤프를
// 요청하면 크래시 난 스레드가 쥔 mutex 를 기다려 시간 제한(10초)이 다 지나야 끝난다.
TEST(CrashDumpDeathTest, ExitsWhenDumpThreadCrashes)
{
    if (!CanSeeCrashes())
    {
        GTEST_SKIP() << "--gtest_catch_exceptions=0 으로 실행할 때만 확인한다";
    }
    const auto dir = TestDir("dumpthread");
    std::filesystem::remove_all(dir);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EXIT(
        {
            InstallOnce({.directory = dir,
                         .afterCrash = [] { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); },
                         .afterCrashTimeout = std::chrono::seconds{10}});
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        },
        [](int32_t) { return true; }, "");
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
    EXPECT_TRUE(HasDump(dir, L"_crash.dmp")); // 처음 크래시의 덤프는 남는다
}
