using System.Globalization;
using System.Text;

namespace Metrics;

// MetricsCollector 가 모은 지표를 로그 한 줄 글로 만든다. 서버가 Scheduler 로 10초마다 불러 로그에 남긴다.
// C++ Metrics::MetricsReporter 와 같은 역할, 같은 줄 모양이다.
//
//     var reporter = new MetricsReporter(collector);
//     reporter.AddHistogram("tick.latency");
//     reporter.AddCounter("packets.received");
//     scheduler.Every(TimeSpan.FromSeconds(10), zoneLane, () => log.Information("{Metrics}", reporter.Report()));
//     → tick.latency=[n=600 p50=812.0us p99=3.10ms max=12.00ms] packets.received=1200/s
//
// - 히스토그램은 SnapshotAndReset: 지난 Report 뒤 구간의 분포다(n=0 이면 그 구간에 기록이 없었다).
// - 카운터는 지난 Report 뒤 늘어난 양을 그 사이 시간으로 나눈 초당 개수다. 첫 구간은 Add 한 때부터다(그 전에 늘어난 양은 세지 않는다).
// - 글만 만들고 로거는 모른다(카테고리와 출력은 서버가 정한다).
// - 등록한 순서대로 쓴다. Add 는 Report 를 시작하기 전에 끝낸다. Report 는 한 곳(Lane 하나)에서만 부른다.
//
// C++ 과 다른 점: C++ 은 지표 객체를 받아 Add 하나로 나누지만, C# 은 BCL 계기를 이름으로 모으므로(MetricsCollector)
// 이름만 받는다. 이름만으로는 종류를 알 수 없어 AddCounter·AddHistogram 으로 나눴다. 시계는 TimeProvider 로 받는다
// (테스트는 FakeTimeProvider).
public sealed class MetricsReporter
{
    private readonly MetricsCollector _collector;
    private readonly TimeProvider _time;
    private readonly List<Entry> _entries = [];

    // timeProvider 를 주지 않으면 TimeProvider.System(실제 시계).
    public MetricsReporter(MetricsCollector collector, TimeProvider? timeProvider = null)
    {
        ArgumentNullException.ThrowIfNull(collector);
        _collector = collector;
        _time = timeProvider ?? TimeProvider.System;
    }

    public void AddCounter(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        _entries.Add(new Entry(name, isCounter: true) { LastValue = _collector.CounterValue(name), LastTime = _time.GetTimestamp() });
    }

    public void AddHistogram(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        _entries.Add(new Entry(name, isCounter: false));
    }

    // 지난 Report(처음이면 Add 한 때) 뒤 구간을 한 줄로
    public string Report()
    {
        var now = _time.GetTimestamp();
        var line = new StringBuilder();
        foreach (var entry in _entries)
        {
            if (line.Length > 0)
            {
                line.Append(' ');
            }
            if (entry.IsCounter)
            {
                var value = _collector.CounterValue(entry.Name);
                var delta = value - entry.LastValue;
                var seconds = _time.GetElapsedTime(entry.LastTime, now).TotalSeconds;
                entry.LastValue = value;
                entry.LastTime = now;
                // 시간이 흐르지 않았으면(같은 시각에 두 번) 초당 개수를 낼 수 없어 0 으로 쓴다
                var perSecond = seconds > 0 ? delta / seconds : 0;
                line.Append(CultureInfo.InvariantCulture, $"{entry.Name}={perSecond:F0}/s");
                continue;
            }
            var snapshot = _collector.SnapshotAndReset(entry.Name);
            if (snapshot.Count == 0)
            {
                line.Append(CultureInfo.InvariantCulture, $"{entry.Name}=[n=0]");
                continue;
            }
            line.Append(CultureInfo.InvariantCulture,
                $"{entry.Name}=[n={snapshot.Count} p50={FormatDuration(snapshot.P50)} p99={FormatDuration(snapshot.P99)} max={FormatDuration(snapshot.Max)}]");
        }
        return line.ToString();
    }

    // ns 를 읽기 쉬운 단위로: 850ns, 12.3us, 3.10ms, 1.20s (C++ 과 같은 글)
    public static string FormatDuration(long nanoseconds) => nanoseconds switch
    {
        < 1_000 => string.Create(CultureInfo.InvariantCulture, $"{nanoseconds}ns"),
        < 1_000_000 => string.Create(CultureInfo.InvariantCulture, $"{nanoseconds / 1e3:F1}us"),
        < 1_000_000_000 => string.Create(CultureInfo.InvariantCulture, $"{nanoseconds / 1e6:F2}ms"),
        _ => string.Create(CultureInfo.InvariantCulture, $"{nanoseconds / 1e9:F2}s"),
    };

    private sealed class Entry(string name, bool isCounter)
    {
        public string Name { get; } = name;
        public bool IsCounter { get; } = isCounter;
        public long LastValue { get; set; } // 카운터의 지난 Report(처음이면 Add) 때 값
        public long LastTime { get; set; }  // 그때 TimeProvider 타임스탬프. 카운터마다 둬서 늦게 Add 한 카운터도 첫 구간이 맞다
    }
}
