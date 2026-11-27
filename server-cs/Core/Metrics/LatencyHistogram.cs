using System.Numerics;

namespace Metrics;

// 한 번 읽은 분포. 시간은 모두 ns. 넣은 값이 없으면 모두 0. C++ Metrics::HistogramSnapshot 과 같은 칸.
public readonly record struct HistogramSnapshot(long Count, long Sum, long P50, long P90, long P99, long P999, long Max);

// 걸린 시간(ns)의 분포를 모으는 칸 묶음. C++ Metrics::LatencyHistogram 과 같은 칸 나누기(16 미만은 값 그대로, 그 위는
// 2배 구간 하나를 16칸, 2^36ns 까지 528칸, 오차 6.25% 이하)라 같은 값이면 두 언어의 p99 가 같다.
//
// C# 에서는 코드가 이 클래스에 직접 넣지 않는다. BCL Histogram<long>.Record 로 기록하고, MetricsCollector 가
// MeterListener 로 받아 여기에 넣는다(MetricsCollector.cs). 어느 스레드에서 Record 해도 된다(Interlocked).
public sealed class LatencyHistogram
{
    public const int SubBucketBits = 4;
    public const int SubBucketCount = 1 << SubBucketBits;
    public const int MaxBits = 36;
    public const int BucketCount = (MaxBits - SubBucketBits + 1) * SubBucketCount;

    private readonly long[] _buckets = new long[BucketCount];
    private long _sum;
    private long _max;

    // 걸린 시간 하나(ns)를 넣는다. 0 보다 작으면 0 으로 본다.
    public void Record(long nanoseconds)
    {
        var value = Math.Max(0, nanoseconds);
        Interlocked.Increment(ref _buckets[BucketOf((ulong)value)]);
        Interlocked.Add(ref _sum, value);
        // max: 더 클 때만 바꾼다(대부분은 읽기 한 번으로 끝난다)
        var current = Volatile.Read(ref _max);
        while (value > current)
        {
            var seen = Interlocked.CompareExchange(ref _max, value, current);
            if (seen == current)
            {
                break;
            }
            current = seen;
        }
    }

    // 지금까지 넣은 전부의 분포(비우지 않는다)
    public HistogramSnapshot Snapshot()
    {
        var counts = new long[BucketCount];
        for (var bucket = 0; bucket < BucketCount; bucket++)
        {
            counts[bucket] = Volatile.Read(ref _buckets[bucket]);
        }
        return Summarize(counts, Volatile.Read(ref _sum), Volatile.Read(ref _max));
    }

    // 지난번 이 메서드를 부른 뒤 넣은 것의 분포를 읽고 비운다. 칸마다 Exchange 로 꺼내므로 잃지 않는다
    // (경계에 걸린 값 하나는 개수·합·max 가 서로 다른 구간에 갈 수 있다, C++ 과 같다). 읽는 쪽은 한 스레드만 둔다.
    public HistogramSnapshot SnapshotAndReset()
    {
        var counts = new long[BucketCount];
        for (var bucket = 0; bucket < BucketCount; bucket++)
        {
            counts[bucket] = Interlocked.Exchange(ref _buckets[bucket], 0);
        }
        return Summarize(counts, Interlocked.Exchange(ref _sum, 0), Interlocked.Exchange(ref _max, 0));
    }

    // 값(ns) → 칸 번호
    public static int BucketOf(ulong nanoseconds)
    {
        if (nanoseconds < SubBucketCount)
        {
            return (int)nanoseconds;
        }
        var top = BitOperations.Log2(nanoseconds); // 가장 높은 1 비트 위치(4 이상)
        if (top >= MaxBits)
        {
            return BucketCount - 1;
        }
        var shift = top - SubBucketBits;
        var sub = (int)(nanoseconds >> shift) - SubBucketCount;
        return ((top - SubBucketBits + 1) * SubBucketCount) + sub;
    }

    // 칸 번호 → 그 칸의 가장 큰 값
    public static long UpperBoundOf(int bucket)
    {
        if (bucket < SubBucketCount)
        {
            return bucket;
        }
        var shift = (bucket / SubBucketCount) - 1; // 칸 폭 = 2^shift
        var lower = (long)(SubBucketCount + (bucket % SubBucketCount)) << shift;
        return lower + (1L << shift) - 1;
    }

    private static HistogramSnapshot Summarize(long[] counts, long sum, long max)
    {
        var total = 0L;
        foreach (var count in counts)
        {
            total += count;
        }
        return new HistogramSnapshot(
            total,
            sum,
            Percentile(counts, total, max, 500),
            Percentile(counts, total, max, 900),
            Percentile(counts, total, max, 990),
            Percentile(counts, total, max, 999),
            max);
    }

    // 순위 ceil(q × count) 가 들어 있는 칸의 위쪽 경계(max 를 넘지 않는다. 마지막 칸은 max)
    private static long Percentile(long[] counts, long total, long max, long permille)
    {
        if (total == 0)
        {
            return 0;
        }
        var rank = Math.Max(1, ((total * permille) + 999) / 1000);
        var seen = 0L;
        for (var bucket = 0; bucket < counts.Length; bucket++)
        {
            seen += counts[bucket];
            if (seen >= rank)
            {
                return bucket == counts.Length - 1 ? max : Math.Min(UpperBoundOf(bucket), max);
            }
        }
        return max;
    }
}
