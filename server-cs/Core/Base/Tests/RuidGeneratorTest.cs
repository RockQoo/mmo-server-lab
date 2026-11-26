namespace Base.Tests;

// C++ RuidGeneratorTest 와 같은 내용(죽는 테스트는 C++ 만, RuidTest 주석 참고).
public class RuidGeneratorTest
{
    // 테스트가 직접 움직이는 가짜 시계. 시작할 때 기준 시각 + 1000ms 로 보이고, Set 으로 옮긴다.
    // 타임스탬프 단위를 ms 로 둬서(TimestampFrequency = 1000) 계산이 그대로 보이게 한다.
    private sealed class FakeTime : TimeProvider
    {
        private long _offsetMs;

        public override long TimestampFrequency => 1000;
        public override DateTimeOffset GetUtcNow() => DateTimeOffset.FromUnixTimeMilliseconds(Ruid.EpochUnixMs + 1000);
        public override long GetTimestamp() => Volatile.Read(ref _offsetMs);

        public void Set(long nowMs) => Volatile.Write(ref _offsetMs, nowMs - 1000);
    }

    // 타임스탬프를 515번째로 읽을 때부터 1ms 뒤로 간다(1번은 생성자, 512번은 1000ms 발급, 513번째 Next 가 한 번 막힌 뒤).
    private sealed class StepAfter514Reads : TimeProvider
    {
        private int _reads;

        public override long TimestampFrequency => 1000;
        public override DateTimeOffset GetUtcNow() => DateTimeOffset.FromUnixTimeMilliseconds(Ruid.EpochUnixMs + 1000);
        public override long GetTimestamp() => Interlocked.Increment(ref _reads) <= 514 ? 0 : 1;
    }

    [Fact]
    public void IdCarriesRealmNodeAndTime()
    {
        var generator = new RuidGenerator(3, 7, new FakeTime());
        var id = generator.Next();
        Assert.Equal(1000, id.Timestamp);
        Assert.Equal(3, id.Realm);
        Assert.Equal(7, id.Node);
        Assert.Equal(0, id.Sequence);
    }

    // 같은 ms 안에서는 순번이 오르고, ms 가 바뀌면 0부터 다시 센다.
    [Fact]
    public void SequenceCountsWithinSameMs()
    {
        var time = new FakeTime();
        var generator = new RuidGenerator(1, 0, time);
        Assert.Equal(0, generator.Next().Sequence);
        Assert.Equal(1, generator.Next().Sequence);
        Assert.Equal(2, generator.Next().Sequence);

        time.Set(1001);
        var id = generator.Next();
        Assert.Equal(1001, id.Timestamp);
        Assert.Equal(0, id.Sequence);
    }

    // 같은 ms 에 512개를 다 쓰면 기다렸다가 다음 ms 의 순번 0을 낸다(겹치거나 넘치지 않는다).
    [Fact]
    public void WaitsForNextMsAfter512()
    {
        var generator = new RuidGenerator(1, 0, new StepAfter514Reads());
        for (var i = 0; i <= Ruid.MaxSequence; i++)
        {
            var id = generator.Next();
            Assert.Equal(1000, id.Timestamp);
            Assert.Equal(i, id.Sequence);
        }
        var next = generator.Next();
        Assert.Equal(1001, next.Timestamp);
        Assert.Equal(0, next.Sequence);
        Assert.Equal(1, generator.SequenceWaitCount); // 기다린 호출은 513번째 하나뿐
    }

    // 시계가 뒤로 가도(가짜 시계로 흉내) 마지막 시각을 유지해 겹치지 않고 계속 커진다.
    [Fact]
    public void ClockGoingBackStillIncreases()
    {
        var time = new FakeTime();
        var generator = new RuidGenerator(1, 0, time);
        var before = generator.Next();
        time.Set(990);
        var after = generator.Next();
        Assert.True(before < after);
        Assert.Equal(1000, after.Timestamp);
        Assert.Equal(1, after.Sequence);
    }

    // 실제 시계: 한 스레드에서 연속으로 만들면 늘 앞의 것보다 크다.
    [Fact]
    public void RealClockIdsAlwaysIncrease()
    {
        var generator = new RuidGenerator(1, 0);
        var previous = generator.Next();
        for (var i = 0; i < 20_000; i++)
        {
            var id = generator.Next();
            Assert.True(previous < id);
            previous = id;
        }
    }

    // 핵심 보장: 스레드 200개가 동시에 만들어도 같은 ID 가 하나도 없다. 스레드마다 만든 순서대로 커지는지도 본다.
    // 스레드 풀이 아니라 전용 Thread 200개를 Barrier 로 한꺼번에 출발시켜 경쟁을 최대로 만든다(C++ 과 같은 조건).
    [Fact]
    public void ManyThreadsNeverDuplicate()
    {
        const int ThreadCount = 200;
        const int PerThread = 2_500; // 총 50만 개(ms 당 512개 상한이라 1초 남짓 걸린다)

        var generator = new RuidGenerator(5, 9);
        var results = new Ruid[ThreadCount][];
        using var start = new Barrier(ThreadCount);
        var threads = new Thread[ThreadCount];
        for (var t = 0; t < ThreadCount; t++)
        {
            var index = t;
            threads[t] = new Thread(() =>
            {
                var mine = new Ruid[PerThread];
                start.SignalAndWait();
                for (var i = 0; i < PerThread; i++)
                {
                    mine[i] = generator.Next();
                }
                results[index] = mine;
            });
            threads[t].Start();
        }
        foreach (var thread in threads)
        {
            thread.Join();
        }

        var all = new List<Ruid>(ThreadCount * PerThread);
        foreach (var mine in results)
        {
            for (var i = 1; i < mine.Length; i++)
            {
                Assert.True(mine[i - 1] < mine[i], "한 스레드 안에서 커지지 않은 ID 가 있다");
            }
            all.AddRange(mine);
        }

        all.Sort();
        for (var i = 1; i < all.Count; i++)
        {
            Assert.True(all[i - 1] != all[i], $"중복 ID: {all[i]}");
        }
        Assert.All(all, id => Assert.Equal((5, 9), (id.Realm, id.Node)));
    }

    // 노드 번호가 다르면 같은 ms, 같은 순번이어도 겹치지 않는다.
    [Fact]
    public void DifferentNodesNeverCollide()
    {
        var time = new FakeTime();
        var nodeA = new RuidGenerator(1, 1, time);
        var nodeB = new RuidGenerator(1, 2, time);
        var seen = new HashSet<Ruid>();
        for (var i = 0; i < 300; i++)
        {
            Assert.True(seen.Add(nodeA.Next()));
            Assert.True(seen.Add(nodeB.Next()));
        }
    }

    // 주의사항 1을 보여 준다: 노드 번호가 같은 두 프로세스(여기선 두 발급기)는 같은 ID 를 낸다.
    [Fact]
    public void SameNodeCollidesWhichIsWhyNodesMustDiffer()
    {
        var time = new FakeTime();
        var processA = new RuidGenerator(1, 1, time);
        var processB = new RuidGenerator(1, 1, time);
        Assert.Equal(processA.Next(), processB.Next());
    }

    // 전역 발급기는 프로세스에 한 번만 만든다. 테스트 프로세스 안에서 이 테스트만 부른다.
    [Fact]
    public void GlobalAfterInitialize()
    {
        RuidGenerator.InitializeGlobal(2, 4);
        var id = RuidGenerator.Global.Next();
        Assert.Equal(2, id.Realm);
        Assert.Equal(4, id.Node);
    }
}
