using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Base;

// Ruid 를 발급한다. 여러 스레드가 동시에 Next() 를 불러도 같은 값이 나오지 않는다. C++ Base::RuidGenerator 와 같은 역할이다.
//
// 방식: 마지막으로 낸 (시각 << 9 | 순번)을 long 하나에 담고 Interlocked.CompareExchange(CAS)로 바꾼다. 바꾸는 데 성공한
// 스레드만 그 값을 가져가므로 같은 (시각, 순번)은 두 번 나오지 않는다. 락이 없다(경쟁하면 재시도한다).
// 같은 ms 에 512개를 다 쓰면 다음 ms 까지 양보(Thread.Yield)하며 기다린다.
//
// 시계: BCL TimeProvider. 시작할 때 실제 시각(GetUtcNow)을 한 번 읽고, 그 뒤로는 뒤로 가지 않는 타임스탬프(GetTimestamp,
// 기본은 Stopwatch)로 흐른 시간만 더한다. 그래서 실행 중에 시스템 시계가 뒤로 가도 ID 가 겹치지 않는다.
// 테스트는 TimeProvider 를 상속한 가짜 시계를 넣는다(C++ 은 Clock 함수를 넣는다).
//
// 쓰는 법: 서버 Main 에서 InitializeGlobal(realm, node) 를 한 번 부르고, 어디서든 RuidGenerator.Global.Next().
//
// 주의사항 (C++ RuidGenerator.h 와 같다)
//   1. 같은 렐름에서 동시에 떠 있는 프로세스끼리 node 가 달라야 한다. CAS 는 프로세스 안의 스레드끼리만 막아 준다.
//   2. 재시작 사이에 시스템 시계가 뒤로 가 있으면 꺼지기 전에 낸 ID 와 겹칠 수 있다. DB 단계에서 시작 검사를 붙인다.
//   3. 오래 켜 두면 ID 안의 시각이 실제 시각과 몇 초 벌어질 수 있다. 순서(나중 ID 가 크다)는 그대로 지켜진다.
//   4. 한 프로세스가 ms 당 512개를 넘게 만들면 모든 스레드가 다음 ms 까지 기다린다. 대량 발급은 나눠서 한다.
public sealed class RuidGenerator
{
    private const long SequenceMask = Ruid.MaxSequence;

    private static RuidGenerator? s_global;
    private static int s_globalClaimed;

    private readonly TimeProvider _time;
    private readonly long _startElapsedMs;
    private readonly long _startTimestamp;
    private long _sequenceWaits;

    // 마지막으로 낸 (시각 << 9 | 순번). -1은 "아직 안 냄". 모든 스레드가 CAS 로 쓰는 값이라 캐시 라인에 혼자 둔다.
    private PaddedLong _last = new() { Value = -1 };

    // 범위를 벗어난 realm(1~127), node(0~63)는 Trace.Assert 로 끝낸다. timeProvider 를 주지 않으면 TimeProvider.System.
    public RuidGenerator(int realm, int node, TimeProvider? timeProvider = null)
    {
        Trace.Assert(realm is >= Ruid.MinRealm and <= Ruid.MaxRealm, "RuidGenerator realm out of range");
        Trace.Assert(node is >= 0 and <= Ruid.MaxNode, "RuidGenerator node out of range");

        Realm = realm;
        Node = node;
        _time = timeProvider ?? TimeProvider.System;
        _startElapsedMs = _time.GetUtcNow().ToUnixTimeMilliseconds() - Ruid.EpochUnixMs;
        _startTimestamp = _time.GetTimestamp();
    }

    public int Realm { get; }
    public int Node { get; }

    // 같은 ms 의 순번 512개를 다 써서 기다린 횟수. 0이 아니면 대량 발급을 나눌지 본다.
    public long SequenceWaitCount => Interlocked.Read(ref _sequenceWaits);

    // 프로세스 전역 발급기. InitializeGlobal 전에 꺼내면 Trace.Assert(Init 을 빼먹은 프로세스가 아무 번호로 발급하지 않게).
    public static RuidGenerator Global
    {
        get
        {
            var generator = Volatile.Read(ref s_global);
            Trace.Assert(generator is not null, "RuidGenerator.Global used before InitializeGlobal");
            return generator!;
        }
    }

    // Main 에서 한 번만 부른다. 두 번째 호출을 허용하면 번호가 바뀌거나 순번이 처음부터 다시 나가 이미 낸 ID 와 겹친다.
    public static void InitializeGlobal(int realm, int node)
    {
        var alreadyClaimed = Interlocked.Exchange(ref s_globalClaimed, 1) != 0;
        Trace.Assert(!alreadyClaimed, "RuidGenerator.InitializeGlobal called twice");
        Volatile.Write(ref s_global, new RuidGenerator(realm, node));
    }

    // 새 ID. 이 발급기가 낸 ID 는 부른 순서대로 커진다(한 스레드 안에서는 항상 앞의 것보다 크다).
    // 시계가 범위(기준 시각 ~ 69.7년)를 벗어나면 Trace.Assert 로 끝낸다.
    public Ruid Next()
    {
        var last = Volatile.Read(ref _last.Value);
        while (true)
        {
            var now = NowMs();
            Trace.Assert(now is >= 0 and <= Ruid.MaxTimestamp, "Ruid clock out of range");

            var lastTime = last >> Ruid.SequenceBits; // -1이면 산술 시프트로 -1
            var lastSequence = last & SequenceMask;

            var time = now;
            var sequence = 0L;
            if (now <= lastTime)
            {
                // 같은 ms. (now < lastTime 은 기본 시계에선 없고, 가짜 시계가 뒤로 갈 때만 생긴다.
                // 그때도 마지막 시각을 유지해 겹치지 않고 커지게 한다)
                time = lastTime;
                if (lastSequence == SequenceMask)
                {
                    // 이 ms 의 512개를 다 썼다. 다음 ms 가 될 때까지 양보한다.
                    Interlocked.Increment(ref _sequenceWaits);
                    Thread.Yield();
                    last = Volatile.Read(ref _last.Value);
                    continue;
                }
                sequence = lastSequence + 1;
            }

            var next = (time << Ruid.SequenceBits) | sequence;
            // 실패하면 지금 값을 돌려받는다. 그 값으로 다시 계산한다.
            var observed = Interlocked.CompareExchange(ref _last.Value, next, last);
            if (observed == last)
            {
                return Ruid.Make(time, Realm, Node, (int)sequence);
            }
            last = observed;
        }
    }

    // 기준 시각부터 지난 ms. 시작 시각 + 뒤로 가지 않는 타임스탬프로 흐른 시간.
    private long NowMs() => _startElapsedMs + (long)_time.GetElapsedTime(_startTimestamp).TotalMilliseconds;

    // long 하나를 캐시 라인(64바이트) 사이에 끼워 다른 필드와 같은 캐시 라인을 쓰지 않게 한다(거짓 공유 방지).
    // C++ 의 alignas(std::hardware_destructive_interference_size) 자리. C# 은 정렬을 지정할 수 없어 앞뒤를 채운다.
    [StructLayout(LayoutKind.Explicit, Size = 128)]
    private struct PaddedLong
    {
        [FieldOffset(64)]
        public long Value;
    }
}
