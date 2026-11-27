using System.Buffers;
using BenchmarkDotNet.Attributes;
using Microsoft.Extensions.ObjectPool;

namespace Bench;

// 빌리는 스레드 N개 → 반납하는 스레드 M개(C++ BufferPoolBench·ObjectPoolBench 의 RentReturn 과 같은 틀·횟수·조합).
// 틀: Bench/RentReturnGrid.cs. Mean = 전체 걸린 시간 / 전체 횟수(빌리기 + 반납 한 쌍이 한 번).
// 조합은 실행하는 PC의 논리 코어 수 C 로 정한다(C++ Bench::RentReturnShapes 와 같다):
//   여유(C/2): 1:1(기준선), 1:C/2, C/2:1, C/4:C/4 / 최대(C): 1:C, C:1, C/2:C/2 / 초과(1.25C): 1:1.25C, 1.25C:1, 0.625C:0.625C
//   논리 16 PC 에서는 1:1, 1:8, 8:1, 4:4 / 1:16, 16:1, 8:8 / 1:20, 20:1, 10:10
internal static class RentReturnShapes
{
    // 13,107,200(2^18 x 50, 64 의 배수): C++ Bench::RentReturnOps 와 같은 수. 조합과 상관없이 정확히 이만큼 한다
    // (RentReturnGrid 가 블록을 고르게 나눈다)
    public const int TotalOps = 50 << 18;

    public static IEnumerable<string> All()
    {
        var cores = Math.Max(4, Environment.ProcessorCount);
        var half = cores / 2;
        var quarter = cores / 4;
        var over = cores + cores / 4;
        var overHalf = over / 2;
        return [$"1:1", $"1:{half}", $"{half}:1", $"{quarter}:{quarter}",
            $"1:{cores}", $"{cores}:1", $"{half}:{half}",
            $"1:{over}", $"{over}:1", $"{overHalf}:{overHalf}"];
    }

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

    public static IEnumerable<string> Shapes => RentReturnShapes.All();

    [ParamsSource(nameof(Shapes))]
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
// 비교 비율(Ratio)은 카테고리마다 따로 계산한다(Plain 은 NewPlain, WithList 는 NewWithList 기준. Codex 리뷰 R2).
// Noop: 같은 틀로 미리 만든 객체 하나를 넘기기만 한다. 측정 틀 자체의 비용(Codex 리뷰 R1, 버퍼 벤치도 같은 틀).
[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 3, iterationCount: 5)]
[GroupBenchmarksBy(BenchmarkDotNet.Configs.BenchmarkLogicalGroupRule.ByCategory)]
[CategoriesColumn]
public class ObjectRentReturnBench
{
    private static readonly ObjectPool<Plain> s_plainPool = new DefaultObjectPool<Plain>(new PlainPolicy());
    private static readonly ObjectPool<WithList> s_listPool = new DefaultObjectPool<WithList>(new WithListPolicy());
    private static readonly object s_token = new();

    private RentReturnGrid<object>? _noopGrid;
    private RentReturnGrid<Plain>? _plainGrid;
    private RentReturnGrid<WithList>? _listGrid;

    public static IEnumerable<string> Shapes => RentReturnShapes.All();

    [ParamsSource(nameof(Shapes))]
    public string Shape { get; set; } = "1:1";

    [GlobalSetup]
    public void Setup()
    {
        var (renters, returners) = RentReturnShapes.Parse(Shape);
        _noopGrid = new RentReturnGrid<object>(renters, returners);
        _plainGrid = new RentReturnGrid<Plain>(renters, returners);
        _listGrid = new RentReturnGrid<WithList>(renters, returners);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _noopGrid?.Dispose();
        _plainGrid?.Dispose();
        _listGrid?.Dispose();
    }

    [Benchmark(Baseline = true, OperationsPerInvoke = RentReturnShapes.TotalOps)]
    [BenchmarkCategory("Noop")]
    public void Noop() => _noopGrid!.Run(RentReturnShapes.TotalOps, static () => s_token, static _ => { });

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

    [Benchmark(Baseline = true, OperationsPerInvoke = RentReturnShapes.TotalOps)]
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
