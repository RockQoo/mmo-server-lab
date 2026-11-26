using System.Runtime.CompilerServices;
using BenchmarkDotNet.Attributes;

namespace Bench;

// 측정 환경의 기준: 아무 일도 하지 않는 반복을 스레드 1·2·4·8개로 나눠 돌린다(C++ Bench 의 Noop 과 같은 일).
// 스레드 수만큼 빨라지지 않으면 그 틀로 잰 여러 스레드 결과를 믿지 않는다.
//   Threads      BenchThreads(전용 스레드 N개). ObjectPoolBench 가 쓴다
//   ParallelFor  Parallel.For(MaxDegreeOfParallelism = N). ArrayPoolBench 가 쓴다(실제 동시 수는 보장되지 않음)
public class NoopBench
{
    private const int TotalOps = 1 << 23; // C++ 과 같은 횟수

    [Params(1, 2, 4, 8)]
    public int Threads { get; set; }

    [Benchmark(OperationsPerInvoke = TotalOps)]
    public void DedicatedThreads() => BenchThreads.Run(Threads, TotalOps / Threads, Consume);

    [Benchmark(OperationsPerInvoke = TotalOps)]
    public void ParallelFor()
    {
        var perThread = TotalOps / Threads;
        Parallel.For(0, Threads, new ParallelOptions { MaxDegreeOfParallelism = Threads }, _ =>
        {
            for (var i = 0; i < perThread; ++i)
            {
                Consume(i);
            }
        });
    }

    // 인라인하지 않아 반복이 지워지지 않게 한다.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Consume(int value) => _ = value;
}
