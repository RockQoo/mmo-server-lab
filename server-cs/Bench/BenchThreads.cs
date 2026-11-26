namespace Bench;

// 여러 스레드 측정 틀: 전용 스레드 threads개를 띄워 각자 perThread번 body(반복 번호)를 돌리고 모두 끝날 때까지 기다린다.
// C++ Bench 의 RunOnThreads(스레드 N개 생성 → join)와 같은 방법이다.
// Parallel.For 의 MaxDegreeOfParallelism 은 상한일 뿐이라 짧은 일에서는 작업자 하나가 여러 몫을 처리할 수 있어 실제
// 동시 스레드 수를 보장하지 않는다(Codex 리뷰 R1). 이 틀이 스레드 수만큼 늘어나는지는 NoopBench 로 확인한다.
internal static class BenchThreads
{
    public static void Run(int threads, int perThread, Action<int> body)
    {
        var workers = new Thread[threads];
        for (var t = 0; t < threads; ++t)
        {
            workers[t] = new Thread(() =>
            {
                for (var i = 0; i < perThread; ++i)
                {
                    body(i);
                }
            });
            workers[t].Start();
        }
        foreach (var worker in workers)
        {
            worker.Join();
        }
    }
}
