using BenchmarkDotNet.Attributes;
using Microsoft.Extensions.ObjectPool;

namespace Bench;

// 객체 하나를 빌려 값을 쓰고 바로 돌려주는 비용(C++ Bench/ObjectPoolBench.cpp 의 SameThread 와 같은 일).
// 스레드 1·2·4·8개가 전체 TotalOps 를 나눠 한다. Mean = 전체 걸린 시간 / TotalOps(모든 스레드를 합친 한 번의 시간).
//
// 객체 두 가지(C++ 과 같다)
//   Plain     값만 있다. 풀 자체의 비용을 본다
//   WithList  List<int> 가 있고 하나를 넣는다. new 는 매번 List 와 그 배열도 할당하고, 풀은 Return 에서 Clear()라
//             배열을 다시 쓴다
//
// 비교 대상
//   New         풀 없이 매번 new(기준). 해제는 GC가 한다. Allocated 열에 할당량이 보인다
//   ObjectPool  Microsoft.Extensions.ObjectPool 의 DefaultObjectPool(기본 보관 개수 = 프로세서 수 x 2)
[MemoryDiagnoser]
public class ObjectPoolBench
{
    private const int TotalOps = 80_000; // 1, 2, 4, 8 로 나누어떨어진다

    private static readonly ObjectPool<Plain> s_plainPool = new DefaultObjectPool<Plain>(new PlainPolicy());
    private static readonly ObjectPool<WithList> s_listPool = new DefaultObjectPool<WithList>(new WithListPolicy());

    [Params(1, 2, 4, 8)]
    public int Threads { get; set; }

    [Benchmark(Baseline = true, OperationsPerInvoke = TotalOps)]
    [BenchmarkCategory("Plain")]
    public void NewPlain() => RunOnThreads(static i =>
    {
        var item = new Plain { Id = i };
        GC.KeepAlive(item);
    });

    [Benchmark(OperationsPerInvoke = TotalOps)]
    [BenchmarkCategory("Plain")]
    public void ObjectPoolPlain() => RunOnThreads(static i =>
    {
        var item = s_plainPool.Get();
        try
        {
            item.Id = i;
        }
        finally
        {
            s_plainPool.Return(item);
        }
    });

    [Benchmark(OperationsPerInvoke = TotalOps)]
    [BenchmarkCategory("WithList")]
    public void NewWithList() => RunOnThreads(static i =>
    {
        var item = new WithList { Id = i };
        item.Values.Add(1);
        GC.KeepAlive(item);
    });

    [Benchmark(OperationsPerInvoke = TotalOps)]
    [BenchmarkCategory("WithList")]
    public void ObjectPoolWithList() => RunOnThreads(static i =>
    {
        var item = s_listPool.Get();
        try
        {
            item.Id = i;
            item.Values.Add(1);
        }
        finally
        {
            s_listPool.Return(item);
        }
    });

    private void RunOnThreads(Action<long> op)
    {
        var perThread = TotalOps / Threads;
        Parallel.For(0, Threads, new ParallelOptions { MaxDegreeOfParallelism = Threads }, _ =>
        {
            for (var i = 0; i < perThread; ++i)
            {
                op(i);
            }
        });
    }

    private sealed class Plain
    {
        public long Id { get; set; }
    }

    private sealed class WithList
    {
        public long Id { get; set; }
        public List<int> Values { get; } = [];
    }

    // 반납할 때 값을 비운다(C++ Reset 과 같은 자리). true 면 풀에 남긴다.
    private sealed class PlainPolicy : PooledObjectPolicy<Plain>
    {
        public override Plain Create() => new();

        public override bool Return(Plain obj)
        {
            obj.Id = 0;
            return true;
        }
    }

    private sealed class WithListPolicy : PooledObjectPolicy<WithList>
    {
        public override WithList Create() => new();

        public override bool Return(WithList obj)
        {
            obj.Id = 0;
            obj.Values.Clear(); // 배열(용량)은 남는다
            return true;
        }
    }
}
