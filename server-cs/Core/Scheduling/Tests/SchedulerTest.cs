using System.Runtime.CompilerServices;
using Messaging;
using Microsoft.Extensions.Time.Testing;

namespace Scheduling.Tests;

// FakeTimeProvider 로 시간을 직접 넘긴다(Advance). 타이머 콜백은 Advance 안에서 불리고, 일은 Lane 에서 돌므로
// 확인하기 전에 DrainAsync 로 Lane 에 넣은 일이 끝나기를 기다린다. 일이 쓴 값은 그 뒤에 읽는다.
public sealed class SchedulerTest
{
    private static readonly TimeSpan s_patience = TimeSpan.FromSeconds(2);

    // 지금까지 lane 에 넣은 일이 모두 끝날 때까지 기다린다
    private static async Task DrainAsync(Lane lane)
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(lane.TryPost(done.SetResult));
        await done.Task.WaitAsync(s_patience, TestContext.Current.CancellationToken);
    }

    [Fact]
    public async Task AfterRunsOnceOnGivenLane()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        var count = 0;
        var inLane = false;

        scheduler.After(TimeSpan.FromSeconds(3), lane, () =>
        {
            count++;
            inLane = lane.IsInLane;
        });
        time.Advance(TimeSpan.FromSeconds(2));
        await DrainAsync(lane);
        Assert.Equal(0, count); // 아직 시각 전

        time.Advance(TimeSpan.FromSeconds(1));
        await DrainAsync(lane);
        time.Advance(TimeSpan.FromSeconds(10));
        await DrainAsync(lane);

        Assert.Equal(1, count);
        Assert.True(inLane); // 타이머 콜백이 아니라 Lane 에서
    }

    [Fact]
    public async Task EveryRepeatsUntilCanceled()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        var count = 0;

        var id = scheduler.Every(TimeSpan.FromSeconds(5), lane, () => count++);
        for (var i = 0; i < 3; i++)
        {
            time.Advance(TimeSpan.FromSeconds(5));
            await DrainAsync(lane);
        }
        Assert.Equal(3, count);

        Assert.True(scheduler.Cancel(id));
        Assert.False(scheduler.Cancel(id));
        time.Advance(TimeSpan.FromSeconds(30));
        await DrainAsync(lane);
        Assert.Equal(3, count);
    }

    [Fact]
    public async Task CanceledBeforeDueDoesNotRun()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        var ran = false;

        var id = scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true);
        Assert.True(scheduler.Cancel(id));
        time.Advance(TimeSpan.FromSeconds(5));
        await DrainAsync(lane);

        Assert.False(ran);
        Assert.False(scheduler.Cancel(default));
    }

    // 시각이 돼 Lane 에 이미 넣었지만 Lane 이 바빠 아직 안 돈 회차도 Cancel 이 막는다(버프 만료 직전 해제 같은 경우)
    [Fact]
    public async Task CancelStopsRunAlreadyPostedToBusyLane()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        using var gate = new ManualResetEventSlim();
        Assert.True(lane.TryPost(() => gate.Wait(s_patience))); // Lane 을 막아 둔다
        var ran = false;

        var id = scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true);
        time.Advance(TimeSpan.FromSeconds(1)); // 콜백이 Lane 에 회차를 넣는다(Advance 안에서)
        Assert.True(scheduler.Cancel(id));
        gate.Set();
        await DrainAsync(lane);

        Assert.False(ran);
    }

    [Fact]
    public async Task OneShotCancelAfterRunReturnsFalse()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        var ran = false;

        var id = scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true);
        time.Advance(TimeSpan.FromSeconds(1));
        await DrainAsync(lane);

        Assert.True(ran);
        Assert.False(scheduler.Cancel(id));
    }

    // 주기 일이 자기 Lane 안에서 자기 자신을 지우면 그 뒤로 돌지 않는다(독 데미지가 HP 0 에서 멈춤)
    [Fact]
    public async Task PeriodicJobCanCancelItselfInLane()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        using var scheduler = new Scheduler(time);
        var count = 0;
        var canceled = false;
        TimerId id = default;

        id = scheduler.Every(TimeSpan.FromSeconds(1), lane, () =>
        {
            if (++count == 3)
            {
                canceled = scheduler.Cancel(id);
            }
        });
        for (var i = 0; i < 10; i++)
        {
            time.Advance(TimeSpan.FromSeconds(1));
            await DrainAsync(lane);
        }

        Assert.Equal(3, count);
        Assert.True(canceled);
    }

    [Fact]
    public async Task DisposeDropsPendingAndRejectsNew()
    {
        var time = new FakeTimeProvider();
        await using var lane = new Lane();
        var scheduler = new Scheduler(time);
        var ran = false;

        scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true);
        scheduler.Dispose();
        scheduler.Dispose(); // 두 번 불러도 된다

        Assert.Equal(default, scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true));
        time.Advance(TimeSpan.FromSeconds(5));
        await DrainAsync(lane);
        Assert.False(ran);
    }

    // 실제 시계(TimeProvider.System)로도 돈다: 짧은 시간 하나만 확인한다
    [Fact]
    public async Task RunsWithSystemClock()
    {
        await using var lane = new Lane();
        using var scheduler = new Scheduler();
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        scheduler.After(TimeSpan.FromMilliseconds(10), lane, done.SetResult);

        await done.Task.WaitAsync(s_patience, TestContext.Current.CancellationToken);
    }

    // 닫힌 Lane 이 회차를 거절하면 한 번짜리는 목록에서도 지운다(리뷰 R3): 남아 있으면 Cancel 이 true 를 돌려준다
    [Fact]
    public async Task OneShotRejectedByClosedLaneIsForgotten()
    {
        var time = new FakeTimeProvider();
        var lane = new Lane();
        await lane.DisposeAsync();
        using var scheduler = new Scheduler(time);
        var ran = false;

        var id = scheduler.After(TimeSpan.FromSeconds(1), lane, () => ran = true);
        time.Advance(TimeSpan.FromSeconds(1)); // 콜백의 TryPost 가 false

        Assert.False(scheduler.Cancel(id));
        Assert.False(ran);
    }

    // 타이머를 못 만들면(범위 밖 시간) 예외를 그대로 던지고 등록을 남기지 않는다(리뷰 R4): 일이 붙잡은 객체가 풀린다
    [Fact]
    public async Task FailedTimerCreationLeavesNoEntry()
    {
        await using var lane = new Lane();
        using var scheduler = new Scheduler(); // 실제 시계: 약 49.7일(0xFFFFFFFE ms)보다 긴 시간은 거절한다

        var captured = AddTooLongTimer(scheduler, lane);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        Assert.False(captured.IsAlive);
    }

    // 일이 붙잡을 객체를 이 함수 안에서만 만든다(테스트 함수의 지역 변수가 객체를 살려 두지 않게)
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference AddTooLongTimer(Scheduler scheduler, Lane lane)
    {
        var payload = new object();
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            scheduler.After(TimeSpan.FromDays(100), lane, () => GC.KeepAlive(payload)));
        return new WeakReference(payload);
    }
}
