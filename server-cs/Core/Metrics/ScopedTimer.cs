using System.Diagnostics;
using System.Diagnostics.Metrics;

namespace Metrics;

// 블록에 들어갈 때 시계를 읽고, 나갈 때 걸린 시간(ns)을 BCL Histogram<long> 에 넣는다. C++ Metrics::ScopedTimer 와 같다.
//
//     using (new ScopedTimer(tickLatency))
//     {
//         RunTick();
//     }
//
// 시계는 BCL Stopwatch.GetTimestamp(뒤로 가지 않는 시계, C++ steady_clock 자리). struct 라 using 에서 할당이 없다.
public readonly struct ScopedTimer : IDisposable, IEquatable<ScopedTimer>
{
    private static readonly double s_nanosecondsPerTick = 1_000_000_000.0 / Stopwatch.Frequency;

    private readonly Histogram<long> _histogram;
    private readonly long _start;

    public ScopedTimer(Histogram<long> histogram)
    {
        ArgumentNullException.ThrowIfNull(histogram);
        _histogram = histogram;
        _start = Stopwatch.GetTimestamp();
    }

    public static bool operator ==(ScopedTimer left, ScopedTimer right) => left.Equals(right);

    public static bool operator !=(ScopedTimer left, ScopedTimer right) => !left.Equals(right);

    // TimeSpan(100ns 단위)을 거치지 않고 타임스탬프 차이를 바로 ns 로 바꾼다(Linux 는 시계 단위가 1ns 다)
    public void Dispose() => _histogram?.Record((long)((Stopwatch.GetTimestamp() - _start) * s_nanosecondsPerTick));

    public bool Equals(ScopedTimer other) => ReferenceEquals(_histogram, other._histogram) && _start == other._start;

    public override bool Equals(object? obj) => obj is ScopedTimer other && Equals(other);

    public override int GetHashCode() => HashCode.Combine(_histogram, _start);
}
