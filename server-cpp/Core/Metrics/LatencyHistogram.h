#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// 걸린 시간의 분포(존 틱 시간, 패킷 하나 처리 시간, DB 응답 시간). p50(보통)과 p99(가끔 튀는 1%)를 본다.
// 어느 스레드에서 Record 해도 된다. 읽는 쪽은 10초마다 SnapshotAndReset()으로 그 구간의 분포를 로그에 남긴다.
//
//     Metrics::LatencyHistogram tick;
//     { Metrics::ScopedTimer timer{tick}; RunTick(); }   // 블록이 끝나면 걸린 시간을 넣는다(ScopedTimer.h)
//     const auto s = tick.SnapshotAndReset();             // s.p50, s.p99, s.max, s.count (ns)
//
// 칸(로그 구간 버킷): 값(ns)이 16 미만이면 값 그대로 한 칸. 그 위는 2배씩 커지는 구간 하나를 16칸으로 나눈다
// (16~31 은 1ns 칸, 32~63 은 2ns 칸, …, 1ms 근처는 65µs 칸). 그래서 어느 값이든 칸 폭이 값의 1/16 이하라 오차가 6.25%
// 이하다. 2^36ns(약 68.7초)까지 528칸이고, 그보다 큰 값은 마지막 칸에 넣는다(max 는 정확한 값을 따로 든다).
// 메모리는 528 × 8B = 약 4KB 로 정해져 있고 Record 는 칸 번호 계산 + atomic 더하기 하나라 O(1)이다.
//
// 분위수(p50 등)는 그 순위가 들어 있는 칸의 위쪽 경계 값이다("p99 는 이 값 이하"). 단, 지금까지의 max 보다 크게 말하지
// 않는다. 이름이 BCL System.Diagnostics.Metrics.Histogram<T> 와 겹치지 않게 LatencyHistogram 이다.
namespace Metrics
{
    // 한 번 읽은 분포. 시간은 모두 ns. 넣은 값이 없으면 모두 0.
    struct HistogramSnapshot
    {
        int64_t count{};
        int64_t sum{}; // 평균 = sum / count
        int64_t p50{};
        int64_t p90{};
        int64_t p99{};
        int64_t p999{};
        int64_t max{};
    };

    class LatencyHistogram
    {
    public:
        static constexpr int32_t SubBucketBits = 4; // 구간 하나를 2^4 = 16칸으로
        static constexpr int32_t SubBucketCount = 1 << SubBucketBits;
        static constexpr int32_t MaxBits = 36; // 2^36ns ≈ 68.7초까지 칸으로 구분한다
        static constexpr std::size_t BucketCount =
            static_cast<std::size_t>(MaxBits - SubBucketBits + 1) * SubBucketCount;

        LatencyHistogram() = default;
        ~LatencyHistogram() = default;
        // 이름 붙은 자리 하나에 두고 가리켜 쓴다(atomic 이라 복사·이동하지 않는다)
        LatencyHistogram(const LatencyHistogram&) = delete;
        LatencyHistogram& operator=(const LatencyHistogram&) = delete;
        LatencyHistogram(LatencyHistogram&&) = delete;
        LatencyHistogram& operator=(LatencyHistogram&&) = delete;

        // 걸린 시간 하나를 넣는다. 0 보다 작으면 0 으로 본다.
        void Record(std::chrono::nanoseconds elapsed) noexcept;

        // 지금까지 넣은 전부의 분포(비우지 않는다)
        [[nodiscard]] HistogramSnapshot Snapshot() const noexcept;

        // 지난번 이 함수를 부른 뒤(처음이면 처음부터) 넣은 것의 분포를 읽고 비운다. 칸마다 exchange 로 꺼내므로 읽는
        // 동안 다른 스레드가 넣은 값은 이번 아니면 다음 구간에 들어가고 잃지 않는다(경계에 걸린 값 하나는 개수·합·max
        // 가 서로 다른 구간에 갈 수 있다. 10초 구간에서는 무시할 만하다). 읽는 쪽은 한 스레드만 둔다.
        HistogramSnapshot SnapshotAndReset() noexcept;

        // 값(ns) → 칸 번호, 칸 번호 → 그 칸의 가장 큰 값. 테스트와 설명용으로 공개한다.
        [[nodiscard]] static std::size_t BucketOf(uint64_t nanoseconds) noexcept;
        [[nodiscard]] static int64_t UpperBoundOf(std::size_t bucket) noexcept;

    private:
        std::array<std::atomic<int64_t>, BucketCount> _buckets{};
        std::atomic<int64_t> _sum{};
        std::atomic<int64_t> _max{};
    };
} // namespace Metrics
