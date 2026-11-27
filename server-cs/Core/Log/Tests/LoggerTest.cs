using System.Globalization;
using Serilog.Core;
using Serilog.Events;
using Serilog.Formatting.Display;

namespace Log.Tests;

// Serilog 의 로거는 전역 하나라서 이 클래스의 테스트는 차례로 돈다(xUnit 은 클래스 안 테스트를 동시에 돌리지 않는다).
public sealed class LoggerTest : IDisposable
{
    private readonly MemorySink _sink = new();

    public void Dispose() => Logger.Shutdown();

    [Fact]
    public void WritesLevelCategoryPropertiesAndThread()
    {
        Logger.Initialize(new LogOptions(), _sink);
        var log = Logger.For(TestCategory.Battle);

        log.Information("skill hit attacker={Attacker} state={State} name={Name}", 7, TestState.Dead, "검사");
        Logger.Shutdown(); // sink 는 비동기라 닫아야(남은 것을 다 써야) 읽을 수 있다(C++ Log::Flush 자리)

        var line = Assert.Single(_sink.Lines);
        Assert.Contains("[INF] [Battle] skill hit attacker=7 state=Dead name=검사 (T", line, StringComparison.Ordinal);
    }

    [Fact]
    public void SkipsBelowMinimumLevel()
    {
        Logger.Initialize(new LogOptions { MinimumLevel = LogEventLevel.Warning }, _sink);
        var log = Logger.For(TestCategory.System);

        log.Information("hidden");
        log.Warning("shown");
        Logger.Shutdown();

        var line = Assert.Single(_sink.Lines);
        Assert.Contains("[WRN] [System] shown", line, StringComparison.Ordinal);
    }

    [Fact]
    public void WritesDailyFile()
    {
        var dir = Path.Combine(Path.GetTempPath(), "mmo-server-lab-cs-FileSink");
        if (Directory.Exists(dir))
        {
            Directory.Delete(dir, recursive: true);
        }
        Logger.Initialize(new LogOptions { FilePath = Path.Combine(dir, "zone-.log") });
        Logger.For(TestCategory.System).Information("to file");
        Logger.Shutdown(); // 닫아야 파일을 읽을 수 있다

        var expected = Path.Combine(dir, $"zone-{DateTime.Now.ToString("yyyyMMdd", CultureInfo.InvariantCulture)}.log");
        Assert.Contains("[INF] [System] to file", File.ReadAllText(expected), StringComparison.Ordinal);
        Directory.Delete(dir, recursive: true);
    }

    // 크래시 처리기: 예외를 Fatal 로 남기고 큐에 남은 로그를 모두 쓴다(C++ LogDeathTest.FlushesQueuedLinesOnCrash)
    [Fact]
    public void WritesQueuedLinesAndExceptionOnCrash()
    {
        const int Lines = 200;
        var slow = new MemorySink { Delay = TimeSpan.FromMilliseconds(1) };
        Logger.Initialize(new LogOptions(), slow);
        var log = Logger.For(TestCategory.System);
        for (var i = 0; i < Lines; i++)
        {
            log.Information("before crash i={I}", i);
        }

        Assert.True(Logger.OnCrash(new InvalidOperationException("boom"), TimeSpan.FromSeconds(10)));

        Assert.Equal(Lines + 1, slow.Lines.Count);
        Assert.Contains("[FTL] [Crash] unhandled exception", slow.Lines[^1], StringComparison.Ordinal);
        Assert.Contains("boom", slow.Lines[^1], StringComparison.Ordinal);
        Assert.Contains($"(T{Environment.CurrentManagedThreadId})", slow.Lines[^1], StringComparison.Ordinal); // 크래시 난 스레드
    }

    // sink 가 끝나지 않아도 시간 제한 뒤 돌아온다(런타임이 덤프를 쓰고 끝낼 수 있게)
    [Fact]
    public void GivesUpCrashFlushAfterTimeout()
    {
        using var gate = new ManualResetEventSlim();
        var stuck = new MemorySink { Gate = gate };
        Logger.Initialize(new LogOptions(), stuck);
        Logger.For(TestCategory.System).Information("stuck");

        Assert.False(Logger.OnCrash(new InvalidOperationException("boom"), TimeSpan.FromMilliseconds(200)));

        gate.Set(); // 정리: sink 를 풀어 Shutdown 이 끝나게 한다
        Logger.Shutdown();
    }

    private enum TestCategory
    {
        System,
        Battle,
    }

    private enum TestState
    {
        Alive,
        Dead,
    }

    // 줄 모양을 확인하려고 Logger 와 같은 템플릿으로 그려 모은다. Emit 은 비동기 sink 의 백그라운드 스레드 하나가
    // 부르고, 테스트는 Shutdown(그 스레드가 끝날 때까지 기다림) 뒤에 읽는다.
    private sealed class MemorySink : ILogEventSink
    {
        private readonly MessageTemplateTextFormatter _formatter = new(Logger.OutputTemplate, CultureInfo.InvariantCulture);

        public List<string> Lines { get; } = [];

        // 한 줄에 걸리는 시간(크래시 때 큐에 줄이 남아 있게)
        public TimeSpan Delay { get; init; }

        // 있으면 열릴 때까지 기다린다(끝나지 않는 sink)
        public ManualResetEventSlim? Gate { get; init; }

        public void Emit(LogEvent logEvent)
        {
            Gate?.Wait();
            if (Delay > TimeSpan.Zero)
            {
                Thread.Sleep(Delay);
            }
            using var writer = new StringWriter(CultureInfo.InvariantCulture);
            _formatter.Format(logEvent, writer);
            Lines.Add(writer.ToString().TrimEnd());
        }
    }
}
