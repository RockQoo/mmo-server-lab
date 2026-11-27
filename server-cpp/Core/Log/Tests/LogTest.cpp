#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Log/Log.h"

namespace
{
    // 서버(게임 레이어)가 정의하는 카테고리의 예
    enum class TestCategory : std::uint8_t
    {
        System,
        Battle,
    };

    enum class TestState : std::uint8_t
    {
        Alive,
        Dead,
    };

    // 쓴 줄을 모아 둔다. 로거가 sink 를 가지고 Shutdown 때 없애므로 줄 목록은 shared_ptr 로 테스트와 나눠 가진다.
    // Write 는 로그 Lane 스레드에서 불린다. 테스트는 Log::Flush()/Shutdown() 뒤에만 읽는다(그 둘이 기다려 준다).
    class MemorySink final : public Log::LogSink
    {
    public:
        explicit MemorySink(std::shared_ptr<std::vector<std::string>> lines) : _lines(std::move(lines))
        {
        }
        void Write(const Log::LogEvent&, const std::string_view line) override
        {
            _lines->emplace_back(line);
        }

    private:
        std::shared_ptr<std::vector<std::string>> _lines;
    };

    // 테스트 하나 동안 로거를 켠다(소멸 = Shutdown). lines 에 쓴 줄이 모인다.
    class LoggerScope
    {
    public:
        explicit LoggerScope(const Log::Level minimumLevel)
        {
            std::vector<std::unique_ptr<Log::LogSink>> sinks;
            sinks.push_back(std::make_unique<MemorySink>(lines));
            Log::Initialize({.minimumLevel = minimumLevel}, std::move(sinks));
        }
        ~LoggerScope()
        {
            Log::Shutdown();
        }
        LoggerScope(const LoggerScope&) = delete;
        LoggerScope& operator=(const LoggerScope&) = delete;
        LoggerScope(LoggerScope&&) = delete;
        LoggerScope& operator=(LoggerScope&&) = delete;

        std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();
    };
} // namespace

TEST(Log, WritesLevelCategoryKeyValuesAndSource)
{
    const LoggerScope scope{Log::Level::Info};
    auto& lines = *scope.lines;
    Log::Info(TestCategory::Battle, "skill hit").KV("attacker", 7).KV("state", TestState::Dead).KV("name", "검사");
    Log::Flush();

    ASSERT_EQ(lines.size(), 1U);
    const auto& line = lines.front();
    EXPECT_NE(line.find("[INF] [Battle] skill hit attacker=7 state=Dead name=검사 (T"), std::string::npos) << line;
    EXPECT_NE(line.find("LogTest.cpp:"), std::string::npos) << line;
}

TEST(Log, SkipsBelowMinimumLevel)
{
    const LoggerScope scope{Log::Level::Warning};
    auto& lines = *scope.lines;
    Log::Info(TestCategory::System, "hidden").KV("value", 1);
    Log::Warning(TestCategory::System, "shown");
    Log::Flush();

    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines.front().find("[WRN] [System] shown"), std::string::npos) << lines.front();
    EXPECT_FALSE(Log::IsEnabled(Log::Level::Info));
    EXPECT_TRUE(Log::IsEnabled(Log::Level::Error));
}

TEST(Log, KeepsOrderFromOneThread)
{
    const LoggerScope scope{Log::Level::Info};
    auto& lines = *scope.lines;
    for (auto i = 0; i < 1000; ++i)
    {
        Log::Info(TestCategory::System, "n").KV("i", i);
    }
    Log::Flush();

    ASSERT_EQ(lines.size(), 1000U);
    for (auto i = 0; i < 1000; ++i)
    {
        const auto expected = std::format("n i={} ", i);
        EXPECT_NE(lines[static_cast<std::size_t>(i)].find(expected), std::string::npos);
    }
}

TEST(Log, LosesNothingFromManyThreads)
{
    const LoggerScope scope{Log::Level::Info};
    auto& lines = *scope.lines;
    {
        std::vector<std::jthread> threads;
        for (auto t = 0; t < 4; ++t)
        {
            threads.emplace_back(
                [t]
                {
                    for (auto i = 0; i < 1000; ++i)
                    {
                        Log::Info(TestCategory::System, "n").KV("t", t).KV("i", i);
                    }
                });
        }
    } // join
    Log::Shutdown(); // 남은 로그를 모두 쓴다

    EXPECT_EQ(lines.size(), 4000U);
}

TEST(LogWithoutLogger, DoesNothing)
{
    EXPECT_FALSE(Log::IsEnabled(Log::Level::Fatal));
    Log::Fatal(TestCategory::System, "dropped").KV("x", 1); // 로거가 없으면 버린다(크래시 없음)
    Log::Flush();
}

namespace
{
    // 현지 시각 날짜·시·분으로 로그 한 줄 정보를 만든다(FileSink 가 날짜를 현지 시각으로 나누는지 본다)
    Log::LogEvent EventAt(const std::chrono::year_month_day date, const std::chrono::hours hour,
                          const std::chrono::minutes minute)
    {
        const auto local = std::chrono::local_days{date} + hour + minute;
        Log::LogEvent event;
        event.time = std::chrono::current_zone()->to_sys(local);
        return event;
    }

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }
} // namespace

TEST(FileSink, WritesOneFilePerLocalDay)
{
    using namespace std::chrono_literals;
    const auto dir = std::filesystem::temp_directory_path() / "mmo-server-lab-FileSink";
    std::filesystem::remove_all(dir);
    {
        Log::FileSink sink{dir / "zone-.log"};
        sink.Write(EventAt(2026y / 11 / 27, 23h, 59min), "a");
        sink.Write(EventAt(2026y / 11 / 28, 0h, 0min), "b");
        sink.Write(EventAt(2026y / 11 / 28, 12h, 0min), "c");
        EXPECT_EQ(sink.CurrentPath(), dir / "zone-20261128.log");
    } // 닫는다

    EXPECT_EQ(ReadAll(dir / "zone-20261127.log"), "a\n");
    EXPECT_EQ(ReadAll(dir / "zone-20261128.log"), "b\nc\n");
    std::filesystem::remove_all(dir);
}

TEST(FileSink, AppendsWhenReopenedOnSameDay)
{
    using namespace std::chrono_literals;
    const auto dir = std::filesystem::temp_directory_path() / "mmo-server-lab-FileSink-append";
    std::filesystem::remove_all(dir);
    {
        Log::FileSink first{dir / "zone-.log"}; // 서버를 켜서 한 줄
        first.Write(EventAt(2026y / 11 / 27, 10h, 0min), "a");
    }
    {
        Log::FileSink second{dir / "zone-.log"}; // 같은 날 다시 켜서 한 줄: 덮어쓰지 않고 뒤에 붙는다
        second.Write(EventAt(2026y / 11 / 27, 11h, 0min), "b");
    }

    EXPECT_EQ(ReadAll(dir / "zone-20261127.log"), "a\nb\n");
    std::filesystem::remove_all(dir);
}
