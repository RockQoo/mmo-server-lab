using System.Diagnostics;
using System.Threading.Channels;

namespace Messaging;

// 일(Action)을 받아 넣은 순서대로 하나씩 처리한다. C++ Messaging::Lane 과 같은 역할이다.
// 한 Lane에 묶인 데이터(존 하나 등)는 그 Lane의 일만 만지므로 락 없이 쓴다.
//
// C++ 과 다른 점(C#다운 방식):
//   큐: BCL(.NET 기본 제공) Channel<T>. 읽는 쪽이 하나(SingleReader)라고 알려 더 가벼운 구현을 쓴다.
//   처리: 전용 스레드 대신 스레드 풀에서 도는 async 루프. 일을 처리하는 스레드는 바뀔 수 있지만
//         "한 번에 하나씩, 넣은 순서대로"는 같다(일이 동시에 두 개 돌지 않는다). 쉬는 동안 스레드를 잡고 있지 않는다.
//   종료: await using / DisposeAsync. 이미 넣은 일을 모두 처리한 뒤 끝난다.
//
// 일이 예외를 던지면 프로세스를 끝낸다(Environment.FailFast, C++ 의 std::terminate 자리).
// 그대로 두면 루프 Task 만 조용히 실패하고 이후 일은 쌓이기만 한다. 반쯤 바뀐 상태로 계속 도는 것보다 안전하다.
public sealed class Lane : IAsyncDisposable
{
    // 지금 이 스레드에서 일을 처리 중인 Lane. 일은 동기 Action 이라 처리하는 동안 스레드가 바뀌지 않는다.
    // 스레드별 static 은 t_ 접두사다. .editorconfig 이름 규칙은 [ThreadStatic] 을 구분하지 못해 s_ 를 요구하므로 여기서만 끈다.
#pragma warning disable IDE1006 // 이름 규칙
    [ThreadStatic]
    private static Lane? t_currentLane;
#pragma warning restore IDE1006

    private readonly Channel<Action> _jobs = Channel.CreateUnbounded<Action>(new UnboundedChannelOptions
    {
        SingleReader = true,
    });
    private readonly Task _loop;

    public Lane()
    {
        _loop = Task.Run(RunAsync);
    }

    // 지금 이 코드가 이 Lane의 일 안에서 도는가. "이 데이터는 이 Lane에서만" 확인(Trace.Assert)에 쓴다.
    public bool IsInLane => t_currentLane == this;

    // 일을 넣는다. 어느 스레드에서 불러도 된다. 멈추는 중(Lane 안의 일에서 넣는 것도 포함)이면 넣지 않고 false.
    // bool 을 돌려주는 메서드라 C# 관례대로 Try 를 붙였다(Channel 의 TryWrite 와 같다). null 은 프로그래머 실수라 예외.
    public bool TryPost(Action job)
    {
        ArgumentNullException.ThrowIfNull(job);
        return _jobs.Writer.TryWrite(job);
    }

    // 새 일을 막고, 이미 넣은 일을 모두 처리할 때까지 기다린다. 두 번 불러도 된다.
    // Lane 자기 일 안에서 부르면 자기 끝을 기다리게 되므로 멈춘다(C++ Stop 의 ASSERT 와 같다).
    public async ValueTask DisposeAsync()
    {
        Trace.Assert(!IsInLane, "Lane.DisposeAsync called from its own job");
        _jobs.Writer.TryComplete();
        await _loop.ConfigureAwait(false);
    }

    private async Task RunAsync()
    {
        var reader = _jobs.Reader;
        // 일이 생길 때까지 스레드 없이 기다리고, 깨어나면 쌓인 만큼 한꺼번에 처리한다(C++ 의 버퍼 교체와 같은 목적).
        // 닫히고(TryComplete) 남은 일도 없으면 false 가 되어 끝난다.
        while (await reader.WaitToReadAsync().ConfigureAwait(false))
        {
            while (reader.TryRead(out var job))
            {
                Run(job);
            }
        }
    }

    private void Run(Action job)
    {
        t_currentLane = this;
        try
        {
            job();
        }
        // 잡아서 계속하려는 것이 아니라 프로세스를 끝내려는 것이다(위 클래스 주석).
#pragma warning disable CA1031 // 모든 예외를 잡지 마라
        catch (Exception ex)
#pragma warning restore CA1031
        {
            Environment.FailFast("Lane job threw an exception", ex);
        }
        finally
        {
            t_currentLane = null;
        }
    }
}
