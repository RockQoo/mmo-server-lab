using System.Diagnostics.Metrics;

namespace Metrics.Tests;

// Meter 이름을 테스트마다 다르게 해서 병렬로 도는 다른 테스트의 값이 섞이지 않게 한다.
public sealed class LatencyHistogramTest
{
    // C++ 테스트와 같은 확인: 모든 값이 자기 칸 안, 칸 폭이 값의 1/16 이하, 칸 번호가 값 순서대로
    [Fact]
    public void BucketsCoverValuesWithinSixPercent()
    {
        var previous = 0;
        for (var value = 0UL; value < (1UL << 36); value = value < 64 ? value + 1 : value + (value / 7))
        {
            var bucket = LatencyHistogram.BucketOf(value);
            Assert.InRange(bucket, previous, LatencyHistogram.BucketCount - 1);
            var upper = LatencyHistogram.UpperBoundOf(bucket);
            Assert.InRange(upper, (long)value, (long)value + ((long)value / 16));
            previous = bucket;
        }
        Assert.Equal(LatencyHistogram.BucketCount - 1, LatencyHistogram.BucketOf(1UL << 40));
    }

    // C++ 과 같은 칸 번호(두 언어의 p99 가 같은 값이 되는 근거)
    [Theory]
    [InlineData(0UL, 0)]
    [InlineData(15UL, 15)]
    [InlineData(16UL, 16)]
    [InlineData(32UL, 32)]
    [InlineData(1_000_000UL, 270)]
    public void BucketNumbersMatchCpp(ulong value, int bucket) =>
        Assert.Equal(bucket, LatencyHistogram.BucketOf(value));

    [Fact]
    public void PercentilesShowRareSpikes()
    {
        var histogram = new LatencyHistogram();
        for (var i = 0; i < 98; i++)
        {
            histogram.Record(1_000_000);
        }
        histogram.Record(50_000_000);
        histogram.Record(50_000_000);

        var snapshot = histogram.Snapshot();

        Assert.InRange(snapshot.P50, 1_000_000, 1_000_000 + 62_500);
        Assert.InRange(snapshot.P99, 50_000_000, 50_000_000 + 3_125_000);
        Assert.Equal(50_000_000, snapshot.Max);
        Assert.Equal(snapshot.Max, snapshot.P999);
    }
}

public sealed class MetricsCollectorTest
{
    [Fact]
    public void SumsCountersFromBcl()
    {
        using var meter = new Meter(nameof(SumsCountersFromBcl));
        var received = meter.CreateCounter<long>("packets.received");
        using var collector = new MetricsCollector(meter.Name);

        received.Add(1);
        received.Add(41);

        Assert.Equal(42, collector.CounterValue("packets.received"));
        Assert.Equal(0, collector.CounterValue("missing"));
    }

    [Fact]
    public void CollectsHistogramIntoIntervals()
    {
        using var meter = new Meter(nameof(CollectsHistogramIntoIntervals));
        using var collector = new MetricsCollector(meter.Name);
        var tick = meter.CreateHistogram<long>("tick.latency", unit: "ns"); // 수집기보다 나중에 만든 계기도 받는다

        tick.Record(2_000_000);
        tick.Record(3_000_000);
        var first = collector.SnapshotAndReset("tick.latency");
        var empty = collector.SnapshotAndReset("tick.latency");
        tick.Record(7_000_000);

        Assert.Equal(2, first.Count);
        Assert.Equal(5_000_000, first.Sum);
        Assert.Equal(3_000_000, first.Max);
        Assert.Equal(0, empty.Count);
        Assert.Equal(1, collector.Snapshot("tick.latency").Count);
    }

    [Fact]
    public void IgnoresOtherMeters()
    {
        using var mine = new Meter(nameof(IgnoresOtherMeters));
        using var other = new Meter(nameof(IgnoresOtherMeters) + ".other");
        using var collector = new MetricsCollector(mine.Name);

        other.CreateCounter<long>("packets.received").Add(5);

        Assert.Equal(0, collector.CounterValue("packets.received"));
    }

    // 여러 스레드가 기록하는 동안 구간마다 꺼내도 하나도 잃지 않는다
    [Fact]
    public async Task KeepsEveryMeasurementFromManyThreads()
    {
        const int Threads = 8;
        const int PerThread = 20_000;
        using var meter = new Meter(nameof(KeepsEveryMeasurementFromManyThreads));
        var counter = meter.CreateCounter<long>("ops");
        var latency = meter.CreateHistogram<long>("op.latency", unit: "ns");
        using var collector = new MetricsCollector(meter.Name);

        var writers = Enumerable.Range(0, Threads).Select(t => Task.Run(() =>
        {
            for (var i = 0; i < PerThread; i++)
            {
                counter.Add(1);
                latency.Record(1000 + i);
            }
        }, TestContext.Current.CancellationToken)).ToArray();
        var collected = 0L;
        while (!writers.All(w => w.IsCompleted))
        {
            collected += collector.SnapshotAndReset("op.latency").Count;
        }
        await Task.WhenAll(writers);
        collected += collector.SnapshotAndReset("op.latency").Count;

        Assert.Equal(Threads * PerThread, collector.CounterValue("ops"));
        Assert.Equal(Threads * PerThread, collected);
    }

    [Fact]
    public void ScopedTimerRecordsElapsedNanoseconds()
    {
        using var meter = new Meter(nameof(ScopedTimerRecordsElapsedNanoseconds));
        var latency = meter.CreateHistogram<long>("block", unit: "ns");
        using var collector = new MetricsCollector(meter.Name);

        using (new ScopedTimer(latency))
        {
            Thread.Sleep(5);
        }

        var snapshot = collector.Snapshot("block");
        Assert.Equal(1, snapshot.Count);
        Assert.InRange(snapshot.Max, 5_000_000, 2_000_000_000);
    }
}
