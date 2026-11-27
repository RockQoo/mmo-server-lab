#include "Log/Log.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <latch>
#include <print>
#include <share.h>
#include <utility>

#include "Base/Assert.h"
#include "Messaging/Lane.h"

namespace Log
{
    namespace
    {
        // Serilog 의 {Level:u3} 와 같은 세 글자
        constexpr std::array<std::string_view, 6> LevelCodes{{"VRB", "DBG", "INF", "WRN", "ERR", "FTL"}};

        // 파일 sink 의 문제를 표준 오류에 알린다. 로그 Lane 안에서 부르므로 예외를 던지지 않는다(밖으로 나가면 Lane
        // 스레드가 terminate, Codex 리뷰 2차 R1). 경로는 UTF-8 로(한글 경로. path::string()은 변환 실패 때 예외).
        void WarnToStderr(const std::string_view message, const std::filesystem::path& path) noexcept
        {
            try
            {
                const auto text = path.u8string();
                std::println(stderr, "Log: {} {}", message,
                             std::string_view{reinterpret_cast<const char*>(text.data()), text.size()});
            }
            catch (...)
            {
                static_cast<void>(std::fputs("Log: file error\n", stderr));
            }
        }

        // 경로에서 파일 이름만(로그 줄을 짧게)
        std::string_view FileName(const std::string_view path) noexcept
        {
            const auto slash = path.find_last_of("\\/");
            return slash == std::string_view::npos ? path : path.substr(slash + 1);
        }

        // 로그 Lane 하나와 sink 들. sink 는 로그 Lane 스레드만 만진다.
        class Logger
        {
        public:
            Logger(const LogOptions& options, std::vector<std::unique_ptr<LogSink>> sinks)
                : _minimumLevel(options.minimumLevel), _sinks(std::move(sinks)), _lane("Log")
            {
            }
            ~Logger()
            {
                _lane.Stop(); // 넣은 로그를 모두 쓰고 스레드가 끝난다. 그 뒤로는 이 스레드에서 sink 를 만져도 된다
                for (const auto& sink : _sinks)
                {
                    sink->Flush();
                }
            }
            Logger(const Logger&) = delete;
            Logger& operator=(const Logger&) = delete;
            Logger(Logger&&) = delete;
            Logger& operator=(Logger&&) = delete;

            [[nodiscard]] bool IsEnabled(const Level level) const noexcept
            {
                return level >= _minimumLevel;
            }

            void Submit(LogEvent&& event)
            {
                // 멈추는 중이면 false: 버린다(Shutdown 뒤의 로그)
                static_cast<void>(_lane.Post([this, event = std::move(event)] { Write(event); }));
            }

            void Flush()
            {
                std::latch done{1};
                const auto posted = _lane.Post(
                    [this, &done]
                    {
                        for (const auto& sink : _sinks)
                        {
                            sink->Flush();
                        }
                        done.count_down();
                    });
                if (posted)
                {
                    done.wait();
                }
            }

        private:
            void Write(const LogEvent& event)
            {
                // 시각은 숫자로 직접 쓴다: chrono 형식 지정({:%Y-%m-%d ...})은 MSVC 에서 로캘 스트림을 거쳐 한 줄에 µs
                // 단위가 들었다(LogBench). 줄 버퍼는 다시 써서 줄마다 할당하지 않는다.
                const auto local = _clock.ToLocal(event.time);
                const auto day = std::chrono::floor<std::chrono::days>(local);
                const std::chrono::year_month_day date{day};
                const std::chrono::hh_mm_ss clock{local - day};
                _line.clear();
                std::format_to(std::back_inserter(_line),
                               "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] [{}] {} (T{}, {}:{})",
                               static_cast<int32_t>(date.year()), static_cast<uint32_t>(date.month()),
                               static_cast<uint32_t>(date.day()), clock.hours().count(), clock.minutes().count(),
                               clock.seconds().count(), clock.subseconds().count(),
                               LevelCodes[static_cast<std::size_t>(event.level)], event.category, event.text,
                               event.threadId, FileName(event.location.file_name()), event.location.line());
                for (const auto& sink : _sinks)
                {
                    sink->Write(event, _line);
                }
            }

            Level _minimumLevel;
            Detail::LocalClock _clock{};
            std::string _line{}; // 한 줄 버퍼(로그 Lane 스레드만 쓴다)
            std::vector<std::unique_ptr<LogSink>> _sinks;
            Messaging::Lane _lane; // 마지막 멤버: 가장 먼저 소멸(스레드 join)
        };

        // Initialize 가 만들고 Shutdown 이 없앤다. 로그를 쓰는 스레드들은 원자 포인터로만 본다.
        std::unique_ptr<Logger> g_owner;
        std::atomic<Logger*> g_logger{};
    } // namespace

    void ConsoleSink::Write(const LogEvent&, const std::string_view line)
    {
        // 로그 Lane 안이라 예외가 나가면 terminate 다(콘솔이 닫힌 경우 등). 콘솔에 못 쓰면 버린다(Codex 리뷰 2차 R1)
        try
        {
            std::println("{}", line);
        }
        catch (...)
        {
            static_cast<void>(std::fputs("Log: console write failed\n", stderr));
        }
    }

    void ConsoleSink::Flush()
    {
        static_cast<void>(std::fflush(stdout));
    }

    FileSink::FileSink(std::filesystem::path pathTemplate) : _pathTemplate(std::move(pathTemplate))
    {
    }

    void FileSink::Write(const LogEvent& event, const std::string_view line)
    {
        const auto day = std::chrono::floor<std::chrono::days>(_clock.ToLocal(event.time));
        if (_currentPath.empty() || day != _day) // 열기에 실패했으면 그날은 다시 시도하지 않는다
        {
            Open(day);
        }
        if (_file == nullptr)
        {
            return;
        }
        const auto written = std::fwrite(line.data(), 1, line.size(), _file.get());
        const auto ok = written == line.size() && std::fputc('\n', _file.get()) != EOF && std::fflush(_file.get()) == 0;
        ReportWriteFailure(ok);
    }

    void FileSink::Flush()
    {
        if (_file != nullptr)
        {
            ReportWriteFailure(std::fflush(_file.get()) == 0);
        }
    }

    // 디스크가 가득 차는 등 쓰기가 실패하면 표준 오류에 알린다. 줄마다 알리지 않게 실패가 시작될 때 한 번만(다시
    // 성공하면 다음 실패 때 또 알린다). 로그를 못 써도 서버는 계속 돈다(Codex 리뷰 R2).
    void FileSink::ReportWriteFailure(const bool ok)
    {
        if (ok)
        {
            _failing = false;
            return;
        }
        if (!_failing)
        {
            _failing = true;
            WarnToStderr("cannot write", _currentPath);
        }
    }

    void FileSink::Open(const std::chrono::local_days day)
    {
        _file.reset();
        _day = day;
        // "zone-.log" → "zone-20261127.log"
        auto name = _pathTemplate.stem().wstring();
        name += std::format(L"{:%Y%m%d}", day);
        name += _pathTemplate.extension().wstring();
        _currentPath = _pathTemplate.parent_path() / name;

        if (_currentPath.has_parent_path())
        {
            std::error_code error;
            std::filesystem::create_directories(_currentPath.parent_path(), error); // 실패하면 아래 열기가 실패한다
        }
        // 다른 프로그램이 쓰는 중에도 읽을 수 있게(_SH_DENYNO, 로그 보기 도구).
        // "ab"(추가 모드)는 CRT 가 쓸 때마다 파일 끝으로 다시 옮겨(seek) 줄마다 시스템 호출이 하나 더 든다. 그래서
        // 있으면 "r+b", 없으면 "wb"로 열고 끝으로 한 번만 옮긴다(로그 Lane 하나만 이 파일에 쓴다).
        _file.reset(_wfsopen(_currentPath.c_str(), L"r+b", _SH_DENYNO));
        if (_file == nullptr)
        {
            _file.reset(_wfsopen(_currentPath.c_str(), L"wb", _SH_DENYNO));
        }
        if (_file != nullptr)
        {
            static_cast<void>(_fseeki64(_file.get(), 0, SEEK_END));
        }
        if (_file == nullptr)
        {
            // 로그 파일을 못 열어도 서버는 계속 돈다. 표준 오류에 한 번 알린다(이 날짜 동안 쓰기는 아무 일 안 함)
            WarnToStderr("cannot open", _currentPath);
        }
    }

    void Initialize(const LogOptions& options, std::vector<std::unique_ptr<LogSink>> sinks)
    {
        ASSERT(g_owner == nullptr, "Log::Initialize called twice");
        g_owner = std::make_unique<Logger>(options, std::move(sinks));
        g_logger.store(g_owner.get(), std::memory_order_release);
    }

    void Shutdown()
    {
        g_logger.store(nullptr, std::memory_order_release);
        g_owner.reset(); // 소멸자가 남은 로그를 모두 쓴다
    }

    void Flush()
    {
        if (auto* const logger = g_logger.load(std::memory_order_acquire))
        {
            logger->Flush();
        }
    }

    bool IsEnabled(const Level level) noexcept
    {
        const auto* const logger = g_logger.load(std::memory_order_acquire);
        return logger != nullptr && logger->IsEnabled(level);
    }

    LogLine::LogLine(const Level level, const std::string_view category, const std::string_view message,
                     const std::source_location& location)
        : _active(IsEnabled(level))
    {
        if (!_active)
        {
            return;
        }
        _event.time = std::chrono::system_clock::now();
        _event.level = level;
        _event.category = category.empty() ? std::string_view{"Unknown"} : category; // magic_enum 범위 밖 값
        _event.text = message;
        _event.threadId = std::this_thread::get_id();
        _event.location = location;
    }

    LogLine::~LogLine()
    {
        if (!_active)
        {
            return;
        }
        if (auto* const logger = g_logger.load(std::memory_order_acquire))
        {
            logger->Submit(std::move(_event));
        }
    }
} // namespace Log
