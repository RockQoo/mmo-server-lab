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

        public void Emit(LogEvent logEvent)
        {
            using var writer = new StringWriter(CultureInfo.InvariantCulture);
            _formatter.Format(logEvent, writer);
            Lines.Add(writer.ToString().TrimEnd());
        }
    }
}
