namespace Messaging.Tests;

// DisposeAsync 가 루프 끝을 기다리므로, 그 뒤에는 일이 쓴 값을 락 없이 읽어도 된다.
public class LaneTest
{
    [Fact]
    public async Task RunsJobsInPostedOrder()
    {
        var order = new List<int>();
        var lane = new Lane();
        for (var i = 0; i < 1000; i++)
        {
            var n = i;
            Assert.True(lane.TryPost(() => order.Add(n)));
        }
        await lane.DisposeAsync();

        Assert.Equal(Enumerable.Range(0, 1000), order);
    }

    [Fact]
    public async Task IsInLaneOnlyInsideJobs()
    {
        var inLane = false;
        var lane = new Lane();
        Assert.True(lane.TryPost(() => inLane = lane.IsInLane));
        await lane.DisposeAsync();

        Assert.True(inLane);
        Assert.False(lane.IsInLane);
    }

    // 다른 Lane의 일 안에서는 false(스레드 풀 스레드를 같이 써도 Lane 끼리 구분된다).
    [Fact]
    public async Task IsInLaneIsFalseInOtherLane()
    {
        var first = new Lane();
        await first.DisposeAsync();
        for (var i = 0; i < 20; i++)
        {
            var inFirst = true;
            var other = new Lane();
            Assert.True(other.TryPost(() => inFirst = first.IsInLane));
            await other.DisposeAsync();
            Assert.False(inFirst);
        }
    }

    // 여러 스레드가 동시에 넣어도 하나도 잃지 않고, 같은 생산자가 넣은 일끼리는 순서가 지켜진다.
    [Fact]
    public async Task KeepsOrderPerProducer()
    {
        const int Producers = 4;
        const int PerProducer = 10000;
        var seen = new List<(int Producer, int Seq)>(); // Lane 의 일만 쓴다
        var lane = new Lane();
        var running = 0; // 일이 동시에 두 개 돌면 1보다 커진다
        var overlapped = false;

        await Task.WhenAll(Enumerable.Range(0, Producers).Select(p => Task.Run(() =>
        {
            for (var n = 0; n < PerProducer; n++)
            {
                var seq = n;
                Assert.True(lane.TryPost(() =>
                {
                    // 일 안에서 Assert 가 던지면 FailFast 로 테스트 러너가 죽는다. 기록만 하고 밖에서 확인한다.
                    overlapped |= Interlocked.Increment(ref running) != 1;
                    seen.Add((p, seq));
                    Interlocked.Decrement(ref running);
                }));
            }
        }, TestContext.Current.CancellationToken)));
        await lane.DisposeAsync();

        Assert.False(overlapped);
        Assert.Equal(Producers * PerProducer, seen.Count);
        var next = new int[Producers];
        foreach (var (p, seq) in seen)
        {
            Assert.Equal(next[p]++, seq);
        }
    }

    // 멈출 때 이미 넣은 일은 버리지 않는다.
    [Fact]
    public async Task DisposeRunsRemainingJobs()
    {
        var done = 0;
        await using (var lane = new Lane())
        {
            for (var i = 0; i < 100; i++)
            {
                Assert.True(lane.TryPost(() => done++));
            }
        }
        Assert.Equal(100, done);
    }

    // 일 안에서 자기 Lane에 다음 일을 넣을 수 있다.
    [Fact]
    public async Task JobCanPostToItsOwnLane()
    {
        var second = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        await using var lane = new Lane();
        var posted = false;
        Assert.True(lane.TryPost(() => posted = lane.TryPost(() => second.SetResult(lane.IsInLane))));

        Assert.True(await second.Task.WaitAsync(TestContext.Current.CancellationToken));
        Assert.True(posted); // 첫 일이 끝난 뒤에 둘째 일이 돌므로 여기서는 이미 쓰여 있다
    }

    [Fact]
    public async Task RejectsAfterDispose()
    {
        var lane = new Lane();
        await lane.DisposeAsync();
        await lane.DisposeAsync(); // 두 번 불러도 된다

        Assert.False(lane.TryPost(() => { }));
    }

    [Fact]
    public async Task NullJobThrows()
    {
        await using var lane = new Lane();
        Assert.Throws<ArgumentNullException>(() => lane.TryPost(null!));
    }
}
