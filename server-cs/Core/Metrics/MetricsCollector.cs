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

    // 카운터 합 하나. C++ MetricCounter 처럼 칸을 나누지 않았다(Interlocked 하나). 여러 스레드 경합 비용은
    // Bench/MetricsBench.cs 가 잰다.
    private sealed class CounterTotal
    {
        private long _value;

        public long Value => Volatile.Read(ref _value);

        public void Add(long delta) => Interlocked.Add(ref _value, delta);
    }
}
