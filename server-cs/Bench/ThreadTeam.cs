namespace Bench;

// 여러 스레드 측정 틀: 전용 스레드 N개를 미리 만들어 두고(GlobalSetup), 측정 한 번마다 모두 같이 출발해서 각자
// perThread번 body(반복 번호)를 돌리고 모두 끝날 때까지 기다린다. C++ Bench/ThreadTeam.h 와 같은 방법이다.
//
// 왜 미리 만드나(Codex 리뷰 2차 R1): 측정마다 스레드를 만들면 생성 비용과 출발 시차(첫 스레드가 뒤 스레드보다 먼저
// 일을 시작)가 측정 시간에 섞인다. 측정 한 번이 몇 ms 로 짧아 Noop 이 8코어 8스레드에서 3.4배만 늘었다.
// 이 틀이 스레드 수만큼 늘어나는지는 NoopBench 로 확인한다.
// (Parallel.For 는 MaxDegreeOfParallelism 이 상한일 뿐이라 실제 동시 스레드 수를 보장하지 않는다. 리뷰 1차 R1)
internal sealed class ThreadTeam : IDisposable
{
    private readonly Thread[] _workers;
    private readonly Barrier _barrier; // 작업자 N + 측정 스레드 1. 출발과 끝에 한 번씩 모두 만난다
    private Action<int> _body = static _ => { };
    private int _perThread;
    private volatile bool _stopping;

    public ThreadTeam(int count)
    {
        _barrier = new Barrier(count + 1);
        _workers = new Thread[count];
        for (var t = 0; t < count; ++t)
        {
            _workers[t] = new Thread(Work) { IsBackground = true, Name = $"bench-{t}" };
            _workers[t].Start();
        }
    }

    // 모든 작업자가 같이 출발해 각자 perThread번 body를 돌리고, 모두 끝나면 돌아온다.
    public void Run(int perThread, Action<int> body)
    {
        _body = body;
        _perThread = perThread;
        _barrier.SignalAndWait(); // 출발(Barrier 가 앞의 쓰기를 작업자에게 보이게 한다)
        _barrier.SignalAndWait(); // 끝
    }

    public void Dispose()
    {
        _stopping = true;
        _barrier.SignalAndWait(); // 작업자가 출발 자리에서 깨어나 멈춘다
        foreach (var worker in _workers)
        {
            worker.Join();
        }
        _barrier.Dispose();
    }

    private void Work()
    {
        while (true)
        {
            _barrier.SignalAndWait();
            if (_stopping)
            {
                return;
            }
            var body = _body;
            var perThread = _perThread;
            for (var i = 0; i < perThread; ++i)
            {
                body(i);
            }
            _barrier.SignalAndWait();
        }
    }
}
