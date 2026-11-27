using System.Diagnostics;
using System.Globalization;
using Serilog;
using Serilog.Core;
using Serilog.Events;

namespace Log;

// 로그 설정과 카테고리별 로거. C++ Log 와 같은 역할이고, 구현은 Serilog 다(C#다운 방식).
//
//   private static readonly ILogger s_log = Logger.For(LogCategory.Battle);
//   s_log.Information("skill hit attacker={Attacker} damage={Damage}", attackerId, 350);
//   → 2026-11-27 14:03:01.123 [INF] [Battle] skill hit attacker=7 damage=350 (T12)
//
// C++ 과 다른 점:
//   키·값: C++ 은 .KV(키, 값), C# 은 Serilog 메시지 템플릿 {Attacker}(같은 속성이 된다). 템플릿을 "키={Key}" 로 쓰면
//          줄 모양이 C++ 과 같다.
//   파일:줄: 붙이지 않는다. Serilog 는 호출 위치를 잡지 않고, C# 의 [CallerFilePath] 는 params 인자와 같이 쓸 수 없다.
//   레벨: 따로 enum 을 만들지 않고 Serilog LogEventLevel 을 쓴다(이름·세 글자 표기가 C++ Level 과 같다).
public static class Logger
{
    // C++ Log 의 줄 모양과 같다(파일:줄만 없다). {Message:l}: 문자열·enum 값을 따옴표 없이 쓴다(기본 :lj 는 enum 을
    // "Dead" 처럼 따옴표로 감싸 C++ 의 state=Dead 와 달라진다).
    public const string OutputTemplate =
        "{Timestamp:yyyy-MM-dd HH:mm:ss.fff} [{Level:u3}] [{Category}] {Message:l} (T{ThreadId}){NewLine}{Exception}";

    private static volatile bool s_initialized;

    // 로거를 만든다. 로그를 쓰는 코드보다 먼저(Main 시작) 한 번 부른다. sinks 는 테스트 등에서 더 붙일 곳(콘솔·파일과 같이 비동기, C++ 과 같다).
    public static void Initialize(LogOptions options, params IEnumerable<ILogEventSink> sinks)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(sinks);
        Trace.Assert(!s_initialized, "Logger.Initialize called twice");

        var config = new LoggerConfiguration()
            .MinimumLevel.Is(options.MinimumLevel)
            .Enrich.With<ThreadIdEnricher>();
        // 모든 sink 는 비동기(Serilog.Sinks.Async): 부른 스레드는 큐에 넣기만 하고 쓰기는 백그라운드 스레드가 한다(C++ 로그
        // Lane 과 같은 구조). 큐가 차면 기다린다(blockWhenFull). 기본값은 버리기라 로그를 잃는다(C++ 은 잃지 않는다).
        config.WriteTo.Async(
            to =>
            {
                if (options.Console)
                {
                    to.Console(outputTemplate: OutputTemplate, formatProvider: CultureInfo.InvariantCulture);
                }
                if (options.FilePath is not null)
                {
                    // "logs/zone-.log" → logs/zone-20261127.log (C++ FileSink 과 같은 이름 규칙)
                    // 크기 상한·보관 개수는 두지 않는다(C++ FileSink 과 같다). Serilog 기본은 파일당 1GB 를 넘으면 그날 나머지를
                    // 버리고 31개보다 오래된 파일을 지운다(Codex 리뷰 R1). 정리는 운영 도구가 한다.
                    to.File(options.FilePath, outputTemplate: OutputTemplate, rollingInterval: RollingInterval.Day,
                        fileSizeLimitBytes: null, retainedFileCountLimit: null, formatProvider: CultureInfo.InvariantCulture);
                }
                foreach (var sink in sinks)
                {
                    to.Sink(sink);
                }
            },
            blockWhenFull: true);
        Serilog.Log.Logger = config.CreateLogger();
        s_initialized = true;
        AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;
    }

    // 넣은 로그를 모두 쓰고 닫는다(C++ Log::Shutdown). 두 번 불러도 된다.
    public static void Shutdown()
    {
        AppDomain.CurrentDomain.UnhandledException -= OnUnhandledException;
        s_initialized = false;
        Serilog.Log.CloseAndFlush();
    }

    // 크래시 때 남은 로그를 기다리는 한도(C++ CrashDumpOptions.afterCrashTimeout 과 같다)
    private static readonly TimeSpan s_crashFlushTimeout = TimeSpan.FromSeconds(1);

    // 처리 안 된 예외(어느 스레드든): 예외를 Fatal 로 남기고 비동기 큐에 남은 로그를 모두 쓴다. 이 처리기가 돌아오면
    // 런타임이 덤프(DOTNET_DbgEnableMiniDump, CrashDump 주석)를 쓰고 프로세스를 끝낸다. C++ 은 덤프 뒤 afterCrash 로
    // Log::Flush 를 부르는 자리다. C++ 처럼 시간 제한을 둔다: 크래시 난 스레드가 sink 가 기다리는 잠금을 쥐었으면 flush
    // 가 끝나지 않는다.
    private static void OnUnhandledException(object? sender, UnhandledExceptionEventArgs e) =>
        OnCrash(e.ExceptionObject as Exception, s_crashFlushTimeout);

    // 끝까지 썼으면 true
    // Fatal 도 시간 제한 안에 넣는다: blockWhenFull 이라 sink 가 막혀 큐가 차 있으면 Fatal 자체가 기다린다(Codex 리뷰 R1).
    internal static bool OnCrash(Exception? exception, TimeSpan timeout) =>
        Task.Run(() =>
        {
            Serilog.Log.ForContext("Category", "Crash").Fatal(exception, "unhandled exception");
            Shutdown();
        }).Wait(timeout);

    // 카테고리가 붙은 로거. Initialize 뒤에 부른다: Serilog 의 ForContext 는 그때의 로거에 묶이므로 앞에서 부르면 영원히
    // 아무것도 쓰지 않는 로거가 된다(조용한 실수를 막으려고 확인한다).
    public static ILogger For<TCategory>(TCategory category) where TCategory : struct, Enum
    {
        Trace.Assert(s_initialized, "Logger.For called before Logger.Initialize");
        return Serilog.Log.ForContext("Category", category.ToString());
    }

    // 줄마다 스레드 ID(C++ 의 T 칸)
    private sealed class ThreadIdEnricher : ILogEventEnricher
    {
        public void Enrich(LogEvent logEvent, ILogEventPropertyFactory propertyFactory) =>
            logEvent.AddPropertyIfAbsent(propertyFactory.CreateProperty("ThreadId", Environment.CurrentManagedThreadId));
    }
}

public sealed record LogOptions
{
    public LogEventLevel MinimumLevel { get; init; } = LogEventLevel.Information;

    // 날짜별 파일. "logs/zone-.log" 를 주면 logs/zone-20261127.log. null 이면 파일에 쓰지 않는다.
    public string? FilePath { get; init; }

    public bool Console { get; init; }
}
