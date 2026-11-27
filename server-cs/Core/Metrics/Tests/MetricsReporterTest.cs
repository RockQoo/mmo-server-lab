using System.Diagnostics.Metrics;
using Microsoft.Extensions.Time.Testing;

namespace Metrics.Tests;

// C++ MetricsReporterTest 와 같은 값 → 같은 글(두 언어의 로그 줄을 그대로 비교할 수 있다)
public sealed class MetricsReporterTest
{
    [Theory]
    [InlineData(850L, "850ns")]
    [InlineData(12_300L, "12.3us")]
    [InlineData(3_100_000L, "3.10ms")]
    [InlineData(1_200_000_000L, "1.20s")]
    public void FormatsDurationInReadableUnit(long nanoseconds, string expected) =>
        Assert.Equal(expected, MetricsReporter.FormatDuration(nanoseconds));

    // 카운터는 구간마다 초당 개수, 히스토그램은 구간마다 분포. 등록한 순서대로 쓴다.
    [Fact]
    public void ReportsEachIntervalInRegistrationOrder()
    {
        using var meter = new Meter(nameof(ReportsEachIntervalInRegistrationOrder));
        var tick = meter.CreateHistogram<long>("tick", unit: "ns");
        var received = meter.CreateCounter<long>("received");
        using var collector = new MetricsCollector(meter.Name);
        var time = new FakeTimeProvider();
        received.Add(999); // 등록 전에 늘어난 양은 세지 않는다

        var reporter = new MetricsReporter(collector, time);
        reporter.AddHistogram("tick");
        reporter.AddCounter("received");

        for (var i = 0; i < 4; i++)
        {
            tick.Record(1_000_000);
        }
        received.Add(100);
        time.Advance(TimeSpan.FromSeconds(10));
        Assert.Equal("tick=[n=4 p50=1.00ms p99=1.00ms max=1.00ms] received=10/s", reporter.Report());
        // 다음 구간: 아무것도 없었다
        time.Advance(TimeSpan.FromSeconds(10));
        Assert.Equal("tick=[n=0] received=0/s", reporter.Report());
    }
}
