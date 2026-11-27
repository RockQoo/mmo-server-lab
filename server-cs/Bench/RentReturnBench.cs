using System.Buffers;
using BenchmarkDotNet.Attributes;
using Microsoft.Extensions.ObjectPool;

namespace Bench;

// 빌리는 스레드 N개 → 반납하는 스레드 M개(C++ BufferPoolBench·ObjectPoolBench 의 RentReturn 과 같은 틀·횟수·조합).
// 틀: Bench/RentReturnGrid.cs. Mean = 전체 걸린 시간 / 전체 횟수(빌리기 + 반납 한 쌍이 한 번).
// 조합: 1:1(기준선) 4:1 1:4 4:4 8:4 4:8 8:8. 8:4·4:8(12스레드), 8:8(16스레드)은 물리 8코어보다 스레드가 많다.
internal static class RentReturnShapes
{
    public const int TotalOps = 1 << 23; // C++ 과 같은 수

    public static (int Renters, int Returners) Parse(string shape)
    {
        var parts = shape.Split(':');
        return (int.Parse(parts[0], System.Globalization.CultureInfo.InvariantCulture),
            int.Parse(parts[1], System.Globalization.CultureInfo.InvariantCulture));
    }
}

// 버퍼: new byte[](반납 = GC) vs BCL ArrayPool<byte>.Shared
[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 3, iterationCount: 5)]
public class BufferRentReturnBench
{
    private RentReturnGrid<byte[]>? _grid;

    [Params("1:1", "4:1", "1:4", "4:4", "8:4", "4:8", "8:8")]
    public string Shape { get; set; } = "1:1";

    [Params(256, 1024, 4096)]
    public int Size { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        var (renters, returners) = RentReturnShapes.Parse(Shape);
        _grid = new RentReturnGrid<byte[]>(renters, returners);
    }

    [GlobalCleanup]
    public void Cleanup() => _grid?.Dispose();

    [Benchmark(Baseline = true, OperationsPerInvoke = RentReturnShapes.TotalOps)]
    public void NewArray()
    {
        var size = Size;
        _grid!.Run(RentReturnShapes.TotalOps, () =>
        {
            var data = new byte[size];
            data[0] = 1;
            return data;
        }, static _ => { });
    }

    [Benchmark(OperationsPerInvoke = RentReturnShapes.TotalOps)]
    public void ArrayPool()
    {
        var size = Size;
        _grid!.Run(RentReturnShapes.TotalOps, () =>
        {
            var data = ArrayPool<byte>.Shared.Rent(size);
            data[0] = 1;
            return data;
        }, static data => ArrayPool<byte>.Shared.Return(data));
    }
}

// 객체: new(반납 = GC) vs Microsoft.Extensions.ObjectPool 의 DefaultObjectPool. 객체 모양은 ObjectPoolBench 와 같다.
[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 3, iterationCount: 5)]
public class ObjectRentReturnBench
{
    private static readonly ObjectPool<Plain> s_plainPool = new DefaultObjectPool<Plain>(new PlainPolicy());
    private static readonly ObjectPool<WithList> s_listPool = new DefaultObjectPool<WithList>(new WithListPolicy());

    private RentReturnGrid<Plain>? _plainGrid;
    private RentReturnGrid<WithList>? _listGrid;

    [Params("1:1", "4:1", "1:4", "4:4", "8:4", "4:8", "8:8")]
    public string Shape { get; set; } = "1:1";

    [GlobalSetup]
    public void Setup()
    {
        var (renters, returners) = RentReturnShapes.Parse(Shape);
        _plainGrid = new RentReturnGrid<Plain>(renters, returners);
        _listGrid = new RentReturnGrid<WithList>(renters, returners);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _plainGrid?.Dispose();
        _listGrid?.Dispose();
    }

    [Benchmark(Baseline = true, OperationsPerInvoke = RentReturnShapes.TotalOps)]
    [BenchmarkCategory("Plain")]
    public void NewPlain() => _plainGrid!.Run(RentReturnShapes.TotalOps, static () => new Plain { Id = 1 }, static _ => { });

    [Benchmark(OperationsPerInvoke = RentReturnShapes.TotalOps)]
    [BenchmarkCategory("Plain")]
    public void ObjectPoolPlain() => _plainGrid!.Run(RentReturnShapes.TotalOps, static () =>
    {
        var item = s_plainPool.Get();
        item.Id = 1;
        return item;
    }, static item => s_plainPool.Return(item));

    [Benchmark(OperationsPerInvoke = RentReturnShapes.TotalOps)]
    [BenchmarkCategory("WithList")]
    public void NewWithList() => _listGrid!.Run(RentReturnShapes.TotalOps, static () =>
    {
        var item = new WithList { Id = 1 };
        item.Values.Add(1);
        return item;
    }, static _ => { });

    [Benchmark(OperationsPerInvoke = RentReturnShapes.TotalOps)]
    [BenchmarkCategory("WithList")]
    public void ObjectPoolWithList() => _listGrid!.Run(RentReturnShapes.TotalOps, static () =>
    {
        var item = s_listPool.Get();
        item.Id = 1;
        item.Values.Add(1);
        return item;
    }, static item => s_listPool.Return(item));

    private sealed class Plain
    {
        public long Id { get; set; }
    }

    private sealed class WithList
    {
        public long Id { get; set; }
        public List<int> Values { get; } = [];
    }

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
            obj.Values.Clear(); // 배열 용량은 남긴다(C++ Reset 과 같은 역할)
            return true;
        }
    }
}
