using System.Diagnostics;
using Messaging;

namespace Scheduling;

// Scheduler 가 발급하는 타이머 번호. 0(default) = 없음.
public readonly record struct TimerId(ulong Value);

// "몇 초 뒤", "몇 초마다" 일을 담당 Lane 에 넣어 준다. C++ Scheduling::Scheduler 와 같은 역할이다.
// 일은 타이머 콜백이 아니라 등록할 때 넘긴 Lane 에서 돈다(그 Lane 의 데이터를 락 없이 만진다).
//
//     using var scheduler = new Scheduler();
//     scheduler.Every(TimeSpan.FromSeconds(10), zoneLane, SaveAll);
//     var id = scheduler.After(TimeSpan.FromSeconds(3), zoneLane, () => { ... });
//     scheduler.Cancel(id);
//
// C++ 과 다른 점(C#다운 방식):
//   - 시각 순서 목록(C++ TimerHeap)과 전용 타이머 스레드가 없다. BCL TimeProvider.CreateTimer(런타임 타이머)가
//     시각이 되면 스레드 풀에서 콜백을 부르고, 콜백은 Lane 에 넣기만 한다.
//   - 시계는 TimeProvider 로 받는다. 테스트는 FakeTimeProvider 로 시간을 직접 넘긴다(기다리지 않는다).
//   - 주기 일의 다음 시각은 런타임 타이머 규칙을 따른다(C++ 처럼 "놓친 칸은 한 번만" 을 따로 보장하지 않는다).
//
// 같은 점: 모든 메서드는 어느 스레드에서 불러도 된다. Cancel 이 true 면 그 뒤로 일이 새로 시작하지 않는다(시각이 돼
// Lane 에 이미 넣었지만 아직 안 돈 회차도 막는다). 다른 스레드에서 Cancel 할 때 Lane 에서 돌고 있는 한 번은 끝까지 돈다.
public sealed class Scheduler : IDisposable
{
    private readonly TimeProvider _time;
    private readonly Lock _lock = new();
    private readonly Dictionary<ulong, Entry> _entries = []; // _lock 이 지킨다. 끝난 한 번짜리는 Lane 에서 지운다
    private ulong _lastId;                                     // _lock 이 지킨다
    private bool _disposed;                                    // _lock 이 지킨다

    // timeProvider 를 주지 않으면 TimeProvider.System(실제 시계)
    public Scheduler(TimeProvider? timeProvider = null)
    {
        _time = timeProvider ?? TimeProvider.System;
    }

    // delay 뒤 한 번 lane 에서 job 을 실행한다. Dispose 뒤면 등록하지 않고 default(0).
    public TimerId After(TimeSpan delay, Lane lane, Action job) => Add(delay, Timeout.InfiniteTimeSpan, lane, job);

    // period 뒤부터 period 마다 lane 에서 job 을 실행한다. period 는 0 보다 커야 한다. Dispose 뒤면 default(0).
    public TimerId Every(TimeSpan period, Lane lane, Action job)
    {
        Trace.Assert(period > TimeSpan.Zero, "Scheduler.Every: period must be positive");
        return Add(period, period, lane, job);
    }

    // 아직 시작하지 않은 실행을 막고 true. 한 번짜리가 이미 돌았거나 없는 id 면 false.
    public bool Cancel(TimerId id)
    {
        Entry? entry;
        lock (_lock)
        {
            if (!_entries.Remove(id.Value, out entry))
            {
                return false;
            }
        }
        entry.Timer?.Dispose(); // 다음 회차를 막는다(이미 Lane 에 넣은 회차는 아래 표시로)
        return entry.TryClaim(); // 먼저 표시한 쪽이 이긴다(한 번짜리가 막 돌기 시작했으면 false)
    }

    // 남은 타이머를 모두 멈춘다. 두 번 불러도 된다. 이미 Lane 에 넣은 회차는 Lane 이 처리한다(C++ Stop 자리).
    public void Dispose()
    {
        Entry[] remaining;
        lock (_lock)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            remaining = [.. _entries.Values];
            _entries.Clear();
        }
        foreach (var entry in remaining)
        {
            entry.Timer?.Dispose();
        }
    }

    private TimerId Add(TimeSpan delay, TimeSpan period, Lane lane, Action job)
    {
        ArgumentNullException.ThrowIfNull(lane);
        ArgumentNullException.ThrowIfNull(job);
        lock (_lock)
        {
            if (_disposed)
            {
                return default;
            }
            var id = ++_lastId;
            var entry = new Entry(this, id, lane, job, periodic: period != Timeout.InfiniteTimeSpan);
            _entries.Add(id, entry);
            // 콜백이 바로(지연 0) 와도 Timer 를 아직 안 넣은 채 쓰지 않는다: 한 번짜리 정리(Finish)도 _lock 을 잡는다
            try
            {
                entry.Timer = _time.CreateTimer(static state => ((Entry)state!).Fire(), entry, delay, period);
            }
            catch
            {
                // 범위 밖 시간(ArgumentOutOfRangeException 등): 호출자는 id 를 못 받아 Cancel 로 지울 수 없으니 되돌린다
                _entries.Remove(id);
                throw;
            }
            return new TimerId(id);
        }
    }

    // 한 번짜리가 Lane 에서 돈 뒤 목록에서 지운다
    private void Finish(Entry entry)
    {
        lock (_lock)
        {
            _entries.Remove(entry.Id);
        }
        entry.Timer?.Dispose();
    }

    // 등록한 일 하나. 타이머 콜백과 Lane 에 넣은 회차가 같이 쓴다.
    private sealed class Entry
    {
        private readonly Scheduler _owner;
        private readonly Lane _lane;
        private readonly Action _job;
        private readonly bool _periodic;
        private readonly Action _runInLane; // 회차마다 대리자를 새로 만들지 않게 한 번만 만든다
        private int _claimed;               // 1 = 지워짐(한 번짜리는 "이미 돌았음"도). Cancel 과 실행 중 먼저 온 쪽만

        public Entry(Scheduler owner, ulong id, Lane lane, Action job, bool periodic)
        {
            _owner = owner;
            Id = id;
            _lane = lane;
            _job = job;
            _periodic = periodic;
            _runInLane = RunInLane;
        }

        public ulong Id { get; }

        public ITimer? Timer { get; set; } // Add 가 _lock 안에서 한 번 넣는다

        // 먼저 표시했으면 true
        public bool TryClaim() => Interlocked.Exchange(ref _claimed, 1) == 0;

        // 타이머 콜백(스레드 풀): Lane 에 넣기만 한다. Lane 이 닫혔으면(종료 중) 이 회차는 버린다.
        // 한 번짜리는 다시 오지 않으므로 그때 목록에서도 지운다(안 지우면 Scheduler 가 살아 있는 동안 일과 캡처가 남는다).
        public void Fire()
        {
            if (!_lane.TryPost(_runInLane) && !_periodic && TryClaim())
            {
                _owner.Finish(this);
            }
        }

        private void RunInLane()
        {
            if (_periodic)
            {
                if (Volatile.Read(ref _claimed) == 0)
                {
                    _job();
                }
                return;
            }
            if (TryClaim())
            {
                _owner.Finish(this);
                _job();
            }
        }
    }
}
