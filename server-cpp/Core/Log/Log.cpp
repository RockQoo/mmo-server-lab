#include "Log/Log.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <latch>
#include <print>
#include <utility>

#include "Base/Assert.h"
#include "Messaging/Lane.h"

namespace Log
{
    namespace
    {
        // Serilog 의 {Level:u3} 와 같은 세 글자
        constexpr std::array<std::string_view, 6> LevelCodes{{"VRB", "DBG", "INF", "WRN", "ERR", "FTL"}};

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
                : _minimumLevel(options.minimumLevel), _zone(std::chrono::current_zone()), _sinks(std::move(sinks)),
                  _lane("Log")
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
                const auto local =
                    std::chrono::zoned_time{_zone, std::chrono::floor<std::chrono::milliseconds>(event.time)};
                const auto line =
                    std::format("{:%Y-%m-%d %H:%M:%S} [{}] [{}] {} (T{}, {}:{})", local,
                                LevelCodes[static_cast<std::size_t>(event.level)], event.category, event.text,
                                event.threadId, FileName(event.location.file_name()), event.location.line());
                for (const auto& sink : _sinks)
                {
                    sink->Write(event, line);
                }
            }

            Level _minimumLevel;
            const std::chrono::time_zone* _zone; // 줄마다 찾지 않게 한 번만
            std::vector<std::unique_ptr<LogSink>> _sinks;
            Messaging::Lane _lane; // 마지막 멤버: 가장 먼저 소멸(스레드 join)
        };

        // Initialize 가 만들고 Shutdown 이 없앤다. 로그를 쓰는 스레드들은 원자 포인터로만 본다.
        std::unique_ptr<Logger> g_owner;
        std::atomic<Logger*> g_logger{};
    } // namespace

    void ConsoleSink::Write(const LogEvent&, const std::string_view line)
    {
        std::println("{}", line);
    }

    void ConsoleSink::Flush()
    {
        static_cast<void>(std::fflush(stdout));
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
