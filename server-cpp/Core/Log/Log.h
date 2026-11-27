#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <magic_enum/magic_enum.hpp>

// 로그. C# 서버의 Serilog 와 같은 역할을 한다(같은 레벨 이름, 같은 줄 모양).
//
//   Log::Info(LogCategory::Battle, "skill hit").KV("attacker", attackerId).KV("damage", 350);
//   → 2026-11-27 14:03:01.123 [INF] [Battle] skill hit attacker=7 damage=350 (T4120, CombatSystem.cpp:88)
//
// - 카테고리는 서버(게임 레이어)가 정의한 enum 이다. Core 는 게임을 모르므로 enum 타입을 받고 이름은 magic_enum 으로
//   얻는다. [Battle] 처럼 카테고리로 골라 보기 좋다.
// - .KV(키, 값)로 칸을 붙인다(Serilog 의 속성). 키로 검색할 수 있다.
// - 비동기: 부른 스레드는 한 줄을 만들어 로그 전용 Lane 에 넣기만 한다. 시각 형식·출력은 Lane 스레드가 한다.
// - 최소 레벨보다 낮으면 아무것도 만들지 않는다(.KV()도 아무 일 안 함).
namespace Log
{
    // 이름과 세 글자 표기(VRB DBG INF WRN ERR FTL)는 Serilog 와 같다.
    enum class Level : std::uint8_t
    {
        Verbose,
        Debug,
        Info,
        Warning,
        Error,
        Fatal,
    };

    // 로그 한 줄. 부른 스레드에서 채우고 로그 Lane 으로 넘긴다.
    struct LogEvent
    {
        std::chrono::system_clock::time_point time{};
        Level level{};
        std::string_view category{}; // magic_enum 이 준 이름(프로그램 끝까지 살아 있는 문자열)
        std::string text{};          // 문장 + " 키=값" 들
        std::thread::id threadId{};
        std::source_location location{};
    };

    // 로그를 쓰는 곳(콘솔, 파일). 로그 Lane 스레드에서만 불리므로 구현에 락이 필요 없다.
    class LogSink
    {
    public:
        virtual ~LogSink() = default;

        // line: 시각·레벨·카테고리까지 붙인 한 줄(줄바꿈 없음). event 는 칸을 따로 쓰고 싶은 sink 용.
        virtual void Write(const LogEvent& event, std::string_view line) = 0;
        virtual void Flush()
        {
        }

    protected:
        // 기반 클래스로만 복사·이동(밖에서 LogSink 로 잘라 복사하지 못하게). 파생 클래스는 따로 적지 않아도 된다.
        LogSink() = default;
        LogSink(const LogSink&) = default;
        LogSink& operator=(const LogSink&) = default;
        LogSink(LogSink&&) = default;
        LogSink& operator=(LogSink&&) = default;
    };

    // 표준 출력. std::println 이 Windows 콘솔에 UTF-8 한글을 바르게 보낸다.
    class ConsoleSink final : public LogSink
    {
    public:
        void Write(const LogEvent& event, std::string_view line) override;
        void Flush() override;
    };

    // 날짜별 파일(Serilog File sink 의 rollingInterval: Day 와 같은 이름 규칙). "logs/zone-.log" 를 주면
    // logs/zone-20261127.log 에 쓰고, 날짜(현지 시각)가 바뀌면 새 파일을 연다. 폴더가 없으면 만든다.
    // 줄마다 flush 한다(Serilog 기본 buffered: false 와 같다. 크래시 직전 줄까지 남는다). 로그 Lane 스레드가 쓰므로
    // 게임 스레드는 기다리지 않는다.
    class FileSink final : public LogSink
    {
    public:
        explicit FileSink(std::filesystem::path pathTemplate);
        ~FileSink() override = default;
        FileSink(const FileSink&) = delete;
        FileSink& operator=(const FileSink&) = delete;
        FileSink(FileSink&&) = delete;
        FileSink& operator=(FileSink&&) = delete;

        void Write(const LogEvent& event, std::string_view line) override;
        void Flush() override;

        // 지금 쓰고 있는 파일(아직 한 줄도 안 썼으면 빈 경로)
        [[nodiscard]] const std::filesystem::path& CurrentPath() const noexcept
        {
            return _currentPath;
        }

    private:
        void Open(std::chrono::local_days day);

        std::filesystem::path _pathTemplate;
        const std::chrono::time_zone* _zone;
        std::chrono::local_days _day{};
        std::filesystem::path _currentPath{};
        std::ofstream _file{};
    };

    struct LogOptions
    {
        Level minimumLevel = Level::Info;
    };

    // 로거를 만든다. 로그를 쓰는 스레드들이 시작하기 전에 한 번 부른다.
    void Initialize(const LogOptions& options, std::vector<std::unique_ptr<LogSink>> sinks);
    // 넣은 로그를 모두 쓰고 로거를 없앤다(Serilog Log.CloseAndFlush). 로그를 쓰는 스레드들이 모두 멈춘 뒤 부른다.
    // 그 뒤의 로그는 버려진다.
    void Shutdown();
    // 지금까지 넣은 로그를 sink 까지 모두 쓸 때까지 기다린다(테스트, 크래시 직전).
    void Flush();
    [[nodiscard]] bool IsEnabled(Level level) noexcept;

    // 한 줄 만들기. Info() 등이 돌려주고, .KV()로 칸을 붙이고, 문장(;)이 끝나 사라질 때 로그 Lane 에 넣는다.
    // 복사·이동을 막아 "문장 하나 = 로그 한 줄"이 되게 한다(돌려줄 때는 C++17 복사 생략으로 이동이 필요 없다).
    class LogLine
    {
    public:
        LogLine(Level level, std::string_view category, std::string_view message, const std::source_location& location);
        ~LogLine();
        LogLine(const LogLine&) = delete;
        LogLine& operator=(const LogLine&) = delete;
        LogLine(LogLine&&) = delete;
        LogLine& operator=(LogLine&&) = delete;

        // " 키=값"을 붙인다. 값은 std::format 으로 쓰고, enum 은 이름으로 쓴다.
        template <typename T>
        LogLine& KV(const std::string_view key, const T& value)
        {
            if (_active)
            {
                if constexpr (std::is_enum_v<T>)
                {
                    std::format_to(std::back_inserter(_event.text), " {}={}", key, magic_enum::enum_name(value));
                }
                else
                {
                    std::format_to(std::back_inserter(_event.text), " {}={}", key, value);
                }
            }
            return *this;
        }

    private:
        bool _active;
        LogEvent _event{};
    };

    namespace Detail
    {
        template <typename Category>
        LogLine Make(const Level level, const Category category, const std::string_view message,
                     const std::source_location& location)
        {
            return LogLine(level, magic_enum::enum_name(category), message, location);
        }
    } // namespace Detail

    // 레벨마다 하나. 파일·줄은 기본 인자 source_location::current()가 부른 곳으로 잡는다(매크로가 필요 없다).
    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Verbose(const Category category, const std::string_view message,
                    const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Verbose, category, message, location);
    }

    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Debug(const Category category, const std::string_view message,
                  const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Debug, category, message, location);
    }

    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Info(const Category category, const std::string_view message,
                 const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Info, category, message, location);
    }

    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Warning(const Category category, const std::string_view message,
                    const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Warning, category, message, location);
    }

    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Error(const Category category, const std::string_view message,
                  const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Error, category, message, location);
    }

    template <typename Category>
        requires std::is_enum_v<Category>
    LogLine Fatal(const Category category, const std::string_view message,
                  const std::source_location& location = std::source_location::current())
    {
        return Detail::Make(Level::Fatal, category, message, location);
    }
} // namespace Log
