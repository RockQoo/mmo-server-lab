using System.Globalization;
using BenchmarkDotNet.Attributes;
using Logger = Log.Logger; // Serilog.Core.Logger 와 이름이 겹친다(sink 를 만드는 파일만 해당)
using LogOptions = Log.LogOptions;
using Serilog.Core;
using Serilog.Events;
using Serilog.Formatting.Display;

namespace Bench;

// 로그 한 줄 비용. C++ Bench/LogBench.cpp 와 같은 줄, 같은 횟수(65,536), 같은 스레드 수(1·4·8).
//   s_log.Information("skill hit attacker={Attacker} damage={Damage}", i, 350)
// 잰 값 두 가지(C++ 과 같은 뜻):
//   EndToEnd: 첫 줄을 넣기 시작해 마지막 줄이 sink 까지 써질 때까지. Serilog 는 비동기 sink 를 비우는 방법이 닫기
//             (CloseAndFlush)뿐이라 닫기까지 잰다(로거 만들기는 IterationSetup 에서, 재지 않음).
//   Caller:   게임 스레드들이 넣기를 마칠 때까지(닫기는 IterationCleanup, 재지 않음). Mean / 스레드 수 가 아니라
//             Mean 이 "전체 줄 수로 나눈 시간"이므로, 스레드 하나가 한 줄에 쓴 시간 = Mean x 스레드 수.
// sink: Null(C++ 과 같이 줄을 그리기만 하고 버림) / File(날짜별 파일). 둘 다 Logger 의 비동기 큐(Serilog.Sinks.Async)를
// 거친다.
[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 3, iterationCount: 10, invocationCount: 1)]
public class LogBench
{
    private const int TotalLines = 1 << 16;

    private static readonly string s_dir = Path.Combine(Path.GetTempPath(), "mmo-server-lab-cs-LogBench");

    private ThreadTeam? _team;
    private Serilog.ILogger? _log;

    [Params(1, 4, 8)]
    public int Threads { get; set; }

    [Params(false, true)]
    public bool ToFile { get; set; }

    [GlobalSetup]
    public void GlobalSetup() => _team = new ThreadTeam(Threads);

    [GlobalCleanup]
    public void GlobalCleanup() => _team?.Dispose();

    [IterationSetup]
    public void IterationSetup()
    {
        if (Directory.Exists(s_dir))
        {
            Directory.Delete(s_dir, recursive: true);
        }
        if (ToFile)
        {
            Logger.Initialize(new LogOptions { FilePath = Path.Combine(s_dir, "bench-.log") });
        }
        else
        {
            Logger.Initialize(new LogOptions(), new NullSink());
        }
        _log = Logger.For(BenchCategory.Battle);
    }

    [IterationCleanup]
    public void IterationCleanup()
    {
        _log = null;
        Logger.Shutdown(); // EndToEnd 는 이미 닫았다(두 번 불러도 된다)
    }

    [Benchmark(OperationsPerInvoke = TotalLines)]
    public void EndToEnd()
    {
        Produce();
        Logger.Shutdown();
    }

    [Benchmark(OperationsPerInvoke = TotalLines)]
    public void Caller() => Produce();

    private void Produce()
    {
        var log = _log!;
        var perThread = TotalLines / Threads;
        _team!.RunEach(_ =>
        {
            for (var i = 0; i < perThread; ++i)
            {
                log.Information("skill hit attacker={Attacker} damage={Damage}", i, 350);
            }
        });
    }

    private enum BenchCategory
    {
        System,
        Battle,
    }

    // C++ NullSink 자리: C++ 로거는 sink 앞에서 줄을 한 번 그리므로, 같게 하려고 그리고 버린다.
    private sealed class NullSink : ILogEventSink
    {
        private readonly MessageTemplateTextFormatter _formatter = new(Logger.OutputTemplate, CultureInfo.InvariantCulture);
        private readonly StringWriter _writer = new(CultureInfo.InvariantCulture);

        public void Emit(LogEvent logEvent)
        {
            _writer.GetStringBuilder().Clear();
            _formatter.Format(logEvent, _writer);
        }
    }
}
