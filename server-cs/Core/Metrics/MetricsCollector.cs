using System.Collections.Concurrent;
using System.Diagnostics.Metrics;

namespace Metrics;

// BCL System.Diagnostics.Metrics 로 기록한 값을 서버 안에서 모아 읽는다. C++ 은 MetricCounter·LatencyHistogram 에 직접
// 넣으므로 이 클래스가 없다(C# 에만 있다).
//
//     var meter = new Meter("mmo.zone");                                  // 서버마다 하나
//     var received = meter.CreateCounter<long>("packets.received");
//     var tick = meter.CreateHistogram<long>("tick.latency", unit: "ns");
//     using var collector = new MetricsCollector("mmo.zone");
//     received.Add(1);  tick.Record(elapsedNs);                           // 어느 스레드에서나
//     collector.CounterValue("packets.received");  collector.SnapshotAndReset("tick.latency");
//
// 왜 BCL 로 기록하나: MS 공식 방식이라 밖에서 dotnet-counters(MS 도구)로 바로 볼 수 있다. 다만 BCL 은 값을 받기만 하고
// 서버 안에서 합이나 p99 를 내 주지 않는다. 그래서 MeterListener 로 같은 값을 받아 C++ 과 같은 칸(LatencyHistogram)에
// 모은다. 받는 콜백은 기록한 스레드에서 바로 불린다(큐 없음).
//
// 받는 것: 이름이 meterName 인 Meter 의 Counter<long>(합) 과 Histogram<long>(ns 분포). 나중에 만든 계기(instrument)도
// 받는다. 다른 Meter 와 다른 타입은 무시한다. 같은 이름의 계기가 둘이면 같은 칸에 모은다.
public sealed class MetricsCollector : IDisposable
{
    private readonly MeterListener _listener = new();
    private readonly ConcurrentDictionary<string, CounterTotal> _counters = new();
    private readonly ConcurrentDictionary<string, LatencyHistogram> _histograms = new();

    public MetricsCollector(string meterName)
    {
        ArgumentException.ThrowIfNullOrEmpty(meterName);
        _listener.InstrumentPublished = (instrument, listener) =>
        {
            if (instrument.Meter.Name != meterName)
            {
                return;
            }
            // 계기마다 모을 곳을 state 로 넘긴다: 값이 올 때마다 이름으로 찾지 않는다
            switch (instrument)
            {
                case Counter<long>:
                    listener.EnableMeasurementEvents(instrument, _counters.GetOrAdd(instrument.Name, _ => new CounterTotal()));
                    break;
                case Histogram<long>:
                    listener.EnableMeasurementEvents(instrument, _histograms.GetOrAdd(instrument.Name, _ => new LatencyHistogram()));
                    break;
                default:
                    break;
            }
        };
        _listener.SetMeasurementEventCallback<long>(static (instrument, measurement, tags, state) =>
        {
            switch (state)
            {
                case CounterTotal total:
                    total.Add(measurement);
                    break;
                case LatencyHistogram histogram:
                    histogram.Record(measurement);
                    break;
                default:
                    break;
            }
        });
        _listener.Start(); // 이미 있는 계기도 InstrumentPublished 로 알려 준다
    }

    // 지금까지 더한 합. 그런 이름의 카운터가 없으면 0.
    public long CounterValue(string name) => _counters.TryGetValue(name, out var total) ? total.Value : 0;

    // 지금까지 넣은 전부의 분포. 그런 이름이 없으면 모두 0.
    public HistogramSnapshot Snapshot(string name) =>
        _histograms.TryGetValue(name, out var histogram) ? histogram.Snapshot() : default;

    // 지난번 뒤로 넣은 것의 분포를 읽고 비운다(LatencyHistogram.SnapshotAndReset). 그런 이름이 없으면 모두 0.
    public HistogramSnapshot SnapshotAndReset(string name) =>
        _histograms.TryGetValue(name, out var histogram) ? histogram.SnapshotAndReset() : default;

    // 받기를 멈춘다. 그 뒤 기록한 값은 모이지 않는다.
    public void Dispose() => _listener.Dispose();

    // 카운터 합 하나. C++ MetricCounter 처럼 캐시 줄마다 띄운 칸 StripeCount 개에 나눠 더하고, 읽을 때 모두 더한다
    // (Interlocked 하나면 여러 스레드가 같은 캐시 줄을 두고 다툰다. 비교는 Bench/MetricsBench.cs).
    // 칸 고르기: C++ 은 스레드마다 정한 번호(thread_local), C# 은 지금 도는 CPU 번호(Thread.GetCurrentProcessorId, BCL).
    // 같은 CPU 에서 도는 스레드는 동시에 돌지 않으므로 다투지 않는다. CPU 가 StripeCount 보다 많으면 칸을 나눠 쓴다.
    // Value 는 C++ 처럼 더하는 동안 올린 것이 들어갈 수도 빠질 수도 있다(늘기만 하므로 다음에 읽으면 들어간다).
    private sealed class CounterTotal
    {
        private const int StripeCount = 16;              // 2의 거듭제곱(& 로 고른다)
        private const int Stride = 64 / sizeof(long);    // 칸 사이 간격 = 캐시 줄 하나(64B)
        // 앞 한 칸은 비운다: 배열 머리(길이, 범위 검사가 읽는다)와 첫 칸이 같은 캐시 줄에 들지 않게
        private readonly long[] _stripes = new long[(StripeCount + 1) * Stride];

        public long Value
        {
            get
            {
                var sum = 0L;
                for (var i = 1; i <= StripeCount; i++)
                {
                    sum += Volatile.Read(ref _stripes[i * Stride]);
                }
                return sum;
            }
        }

        public void Add(long delta)
        {
            var stripe = (Thread.GetCurrentProcessorId() & (StripeCount - 1)) + 1;
            Interlocked.Add(ref _stripes[stripe * Stride], delta);
        }
    }
}
