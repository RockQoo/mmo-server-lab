#pragma once

#include <chrono>

#include "Metrics/LatencyHistogram.h"

// 블록에 들어갈 때 시계를 읽고, 나갈 때 걸린 시간을 히스토그램에 넣는다(예외로 나가도 넣는다).
//
//     {
//         Metrics::ScopedTimer timer{tickLatency};
//         RunTick();
//     } // 여기서 tickLatency.Record(걸린 시간)
//
// 시계는 std::chrono::steady_clock(뒤로 가지 않는 시계). 한 번 읽는 비용은 Bench/LatencyHistogramBench.cpp 가 잰다.
namespace Metrics
{
    class ScopedTimer
    {
    public:
        explicit ScopedTimer(LatencyHistogram& histogram) noexcept
            : _histogram(histogram), _start(std::chrono::steady_clock::now())
        {
        }
        ~ScopedTimer()
        {
            _histogram.Record(std::chrono::steady_clock::now() - _start);
        }

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
        ScopedTimer(ScopedTimer&&) = delete;
        ScopedTimer& operator=(ScopedTimer&&) = delete;

    private:
        LatencyHistogram& _histogram;
        std::chrono::steady_clock::time_point _start;
    };
} // namespace Metrics
