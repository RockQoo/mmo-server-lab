using System.Buffers;
using BenchmarkDotNet.Attributes;

namespace Bench;

// 버퍼 하나를 빌려 한 바이트 쓰고 바로 돌려주는 비용(C++ Bench/BufferPoolBench.cpp 와 같은 일).
// 스레드 1·4·8개가 동시에 같은 일을 한다. 스레드 수와 관계없이 전체 횟수(TotalOps)는 같고,
// 결과의 Mean 은 한 번(빌리기 + 반납)에 걸린 시간 = 전체 걸린 시간 / TotalOps 다.
// (C++ 결과는 스레드마다 한 번에 걸린 시간이라 직접 같은 숫자로 비교하지 않는다. 같은 언어 안에서 비교한다)
//
// 비교 대상
//   NewArray     풀 없이 매번 new byte[](기준). 반납은 GC가 한다. Allocated 열에 할당량이 보인다
//   ArrayPool    BCL ArrayPool<byte>.Shared(스레드별 캐시 + 코어별 묶음)
[MemoryDiagnoser]
public class ArrayPoolBench
{
    private const int TotalOps = 80_000; // 1, 4, 8 로 나누어떨어진다

    [Params(256, 4096)]
    public int Size { get; set; }

    [Params(1, 4, 8)]
    public int Threads { get; set; }

    [Benchmark(Baseline = true, OperationsPerInvoke = TotalOps)]
    public void NewArray() => RunOnThreads(static size =>
    {
        var data = new byte[size];
        data[0] = 1; // 쓰지 않으면 JIT가 할당을 지울 수 있다
        GC.KeepAlive(data);
    });

    [Benchmark(OperationsPerInvoke = TotalOps)]
    public void ArrayPool() => RunOnThreads(static size =>
    {
        var data = ArrayPool<byte>.Shared.Rent(size);
        try
        {
            data[0] = 1;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(data);
        }
    });

    private void RunOnThreads(Action<int> op)
    {
        var size = Size;
        var perThread = TotalOps / Threads;
        Parallel.For(0, Threads, new ParallelOptions { MaxDegreeOfParallelism = Threads }, _ =>
        {
            for (var i = 0; i < perThread; ++i)
            {
                op(size);
            }
        });
    }
}
