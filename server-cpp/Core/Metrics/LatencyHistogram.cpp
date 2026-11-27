#include "Metrics/LatencyHistogram.h"

#include <algorithm>
#include <bit>

namespace Metrics
{
    namespace
    {
        // 칸별 개수로 분위수를 낸다: 순위 ceil(q × count) 가 들어 있는 칸의 위쪽 경계(단, max 를 넘지 않는다)
        int64_t Percentile(const std::array<int64_t, LatencyHistogram::BucketCount>& counts, const int64_t total,
                           const int64_t max, const int64_t permille) noexcept
        {
            if (total == 0)
            {
                return 0;
            }
            const auto rank = std::max(int64_t{1}, (total * permille + 999) / 1000);
            auto seen = int64_t{0};
            for (auto bucket = std::size_t{0}; bucket < counts.size(); ++bucket)
            {
                seen += counts[bucket];
                if (seen >= rank)
                {
                    // 마지막 칸은 범위를 넘은 값도 받으므로 경계 대신 max 로 말한다
                    return bucket + 1 == counts.size() ? max : std::min(LatencyHistogram::UpperBoundOf(bucket), max);
                }
            }
            return max;
        }

        HistogramSnapshot Summarize(const std::array<int64_t, LatencyHistogram::BucketCount>& counts, const int64_t sum,
                                    const int64_t max) noexcept
        {
            auto snapshot = HistogramSnapshot{};
            for (const auto count : counts)
            {
                snapshot.count += count;
            }
            snapshot.sum = sum;
            snapshot.max = max;
            snapshot.p50 = Percentile(counts, snapshot.count, max, 500);
            snapshot.p90 = Percentile(counts, snapshot.count, max, 900);
            snapshot.p99 = Percentile(counts, snapshot.count, max, 990);
            snapshot.p999 = Percentile(counts, snapshot.count, max, 999);
            return snapshot;
        }
    } // namespace

    std::size_t LatencyHistogram::BucketOf(const uint64_t nanoseconds) noexcept
    {
        if (nanoseconds < SubBucketCount)
        {
            return static_cast<std::size_t>(nanoseconds); // 16 미만은 값 그대로
        }
        // 가장 높은 1 비트 위치 e(4 이상). 위 5비트(16~31)에서 16 을 빼면 구간 안 칸 번호다.
        const auto top = std::bit_width(nanoseconds) - 1;
        if (top >= MaxBits)
        {
            return BucketCount - 1;
        }
        const auto shift = top - SubBucketBits;
        const auto sub = static_cast<std::size_t>((nanoseconds >> shift) - SubBucketCount);
        return static_cast<std::size_t>(top - SubBucketBits + 1) * SubBucketCount + sub;
    }

    int64_t LatencyHistogram::UpperBoundOf(const std::size_t bucket) noexcept
    {
        if (bucket < SubBucketCount)
        {
            return static_cast<int64_t>(bucket);
        }
        const auto group = static_cast<int32_t>(bucket / SubBucketCount); // 1 이상
        const auto sub = static_cast<int64_t>(bucket % SubBucketCount);
        const auto shift = group - 1; // 칸 폭 = 2^shift
        const auto lower = (SubBucketCount + sub) << shift;
        return lower + (int64_t{1} << shift) - 1;
    }

    void LatencyHistogram::Record(const std::chrono::nanoseconds elapsed) noexcept
    {
        const auto value = std::max(int64_t{0}, static_cast<int64_t>(elapsed.count()));
        _buckets[BucketOf(static_cast<uint64_t>(value))].fetch_add(1, std::memory_order_relaxed);
        _sum.fetch_add(value, std::memory_order_relaxed);
        // max: 더 클 때만 바꾼다(대부분은 읽기 한 번으로 끝난다)
        auto current = _max.load(std::memory_order_relaxed);
        while (value > current && !_max.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {
        }
    }

    HistogramSnapshot LatencyHistogram::Snapshot() const noexcept
    {
        auto counts = std::array<int64_t, BucketCount>{};
        for (auto bucket = std::size_t{0}; bucket < BucketCount; ++bucket)
        {
            counts[bucket] = _buckets[bucket].load(std::memory_order_relaxed);
        }
        return Summarize(counts, _sum.load(std::memory_order_relaxed), _max.load(std::memory_order_relaxed));
    }

    HistogramSnapshot LatencyHistogram::SnapshotAndReset() noexcept
    {
        auto counts = std::array<int64_t, BucketCount>{};
        for (auto bucket = std::size_t{0}; bucket < BucketCount; ++bucket)
        {
            counts[bucket] = _buckets[bucket].exchange(0, std::memory_order_relaxed);
        }
        return Summarize(counts, _sum.exchange(0, std::memory_order_relaxed),
                         _max.exchange(0, std::memory_order_relaxed));
    }
} // namespace Metrics
