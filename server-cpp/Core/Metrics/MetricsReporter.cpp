#include "Metrics/MetricsReporter.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace Metrics
{
    MetricsReporter::MetricsReporter(const Clock::time_point start) : _lastReport(start)
    {
    }

    void MetricsReporter::Add(std::string name, const MetricCounter& counter)
    {
        _entries.push_back(Entry{.name = std::move(name), .counter = &counter, .lastValue = counter.Value()});
    }

    void MetricsReporter::Add(std::string name, LatencyHistogram& histogram)
    {
        _entries.push_back(Entry{.name = std::move(name), .histogram = &histogram});
    }

    std::string MetricsReporter::Report(const Clock::time_point now)
    {
        const auto seconds = std::chrono::duration<double>(now - _lastReport).count();
        _lastReport = now;

        std::string line;
        for (auto& entry : _entries)
        {
            if (!line.empty())
            {
                line += ' ';
            }
            auto out = std::back_inserter(line);
            if (entry.counter != nullptr)
            {
                const auto value = entry.counter->Value();
                const auto delta = value - entry.lastValue;
                entry.lastValue = value;
                // 시간이 흐르지 않았으면(같은 시각에 두 번) 초당 개수를 낼 수 없어 0 으로 쓴다
                const auto perSecond = seconds > 0.0 ? static_cast<double>(delta) / seconds : 0.0;
                std::format_to(out, "{}={:.0f}/s", entry.name, perSecond);
                continue;
            }
            const auto snapshot = entry.histogram->SnapshotAndReset();
            if (snapshot.count == 0)
            {
                std::format_to(out, "{}=[n=0]", entry.name);
                continue;
            }
            std::format_to(out, "{}=[n={} p50={} p99={} max={}]", entry.name, snapshot.count,
                           FormatDuration(snapshot.p50), FormatDuration(snapshot.p99), FormatDuration(snapshot.max));
        }
        return line;
    }

    std::string MetricsReporter::FormatDuration(const int64_t nanoseconds)
    {
        const auto value = static_cast<double>(nanoseconds);
        if (nanoseconds < 1'000)
        {
            return std::format("{}ns", nanoseconds);
        }
        if (nanoseconds < 1'000'000)
        {
            return std::format("{:.1f}us", value / 1e3);
        }
        if (nanoseconds < 1'000'000'000)
        {
            return std::format("{:.2f}ms", value / 1e6);
        }
        return std::format("{:.2f}s", value / 1e9);
    }
} // namespace Metrics
