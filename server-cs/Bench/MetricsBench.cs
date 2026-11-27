using System.Diagnostics;
using System.Diagnostics.Metrics;
using BenchmarkDotNet.Attributes;
using Metrics;

namespace Bench;

// 지연·카운터 기록 비용(PROGRESS "측정 비용"). C++ Bench/LatencyHistogramBench.cpp·MetricCounterBench.cpp 와 같은 자리.
// BCL 계기(Counter<long>, Histogram<long>)에 기록하면 MetricsCollector 의 MeterListener 콜백이 기록한 스레드에서 바로
// 불려 칸에 넣는다. 그래서 잰 값 = BCL 계기 + 콜백 + Interlocked.
//   GetTimestamp      시계를 한 번 읽는 비용(C++ steady_clock::now 자리)
//   HistogramRecord   Histogram<long>.Record 하나(수집기가 듣는 중)
//   ScopedTimerEmpty  빈 블록 = 시계 두 번 + Record
//   CounterAdd        Counter<long>.Add 를 스레드 N 개가 같이(수집기의 합은 CPU 번호로 고른 칸 16개, C++ MetricCounter 자리). 기준선은 NoopBench.Team
[MemoryDiagnoser]
public class MetricsBench
{
    private const int TotalOps = 1 << 22;

    private Meter? _meter;
    private Counter<long>? _counter;
    private Histogram<long>? _histogram;
    private MetricsCollector? _collector;
    private ThreadTeam? _team;

    [Params(1, 4, 8, 16)]
    public int Threads { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        _meter = new Meter("bench.metrics");
        _counter = _meter.CreateCounter<long>("ops");
        _histogram = _meter.CreateHistogram<long>("latency", unit: "ns");
        _collector = new MetricsCollector(_meter.Name);
        _team = new ThreadTeam(Threads);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _team?.Dispose();
        _collector?.Dispose();
        _meter?.Dispose();
    }

    // BenchmarkDotNet 은 인스턴스 메서드만 잰다(static 으로 바꾸라는 CA1822 를 여기서만 끈다)
#pragma warning disable CA1822
    [Benchmark]
    public long GetTimestamp() => Stopwatch.GetTimestamp();
#pragma warning restore CA1822

    [Benchmark]
    public void HistogramRecord() => _histogram!.Record(1_000_000);

    [Benchmark]
    public void ScopedTimerEmpty()
    {
        using (new ScopedTimer(_histogram!))
        {
        }
    }

    [Benchmark(OperationsPerInvoke = TotalOps)]
    public void CounterAdd() => _team!.Run(TotalOps / Threads, _ => _counter!.Add(1));
}
