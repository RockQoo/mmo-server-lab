using System.Diagnostics;
using System.Globalization;
using Microsoft.Diagnostics.NETCore.Client;

namespace Base;

public enum DumpKind
{
    Mini, // 스레드·스택·관리 힙(관리 코드 분석에 필요). C++ Mini 와 같은 자리
    Full, // 프로세스 메모리 전체
}

// 덤프 파일(.dmp). C++ Base::CrashDump 와 같은 역할이고, C# 은 런타임과 MS 공식 도구를 쓴다.
//
// 자동(크래시): 코드가 아니라 런타임 설정이다. 런타임은 시작할 때만 읽으므로 서버를 띄우는 쪽(실행 스크립트, 서비스
//   설정)이 환경 변수를 준다. 이름은 C++ 과 비슷하게만 된다(%e 실행 파일 이름, .exe 까지 붙는다. %p pid. %t 시각인데
//   날짜가 아니라 1970년부터 센 초다. createdump 에 날짜 형식이 없다. 2026-11-27 실제 덤프로 확인):
//     DOTNET_DbgEnableMiniDump=1
//     DOTNET_DbgMiniDumpType=2            (2 = 관리 힙 포함 = Mini, 4 = Full)
//     DOTNET_DbgMiniDumpName=dumps/%e_%t_pid%p_crash.dmp   → Gateway.exe_1795774681_pid1234_crash.dmp
// 수동(살아 있는 서버): Write(kind, reason). 서버는 계속 돈다. MS 공식 Microsoft.Diagnostics.NETCore.Client 로 자기
//   프로세스에 덤프를 요청한다(dotnet-dump collect 와 같은 경로: 런타임이 createdump 를 띄워 쓴다).
public static class CrashDump
{
    // 같은 초·같은 이유 덤프 이름 뒤에 붙일 번호의 상한(C++ 과 같다)
    private const int MaxSameNameDumps = 100;

    // 일부 복사 실패 뒤 다시 쓰기 전에 기다리는 시간. 덤프를 부른 스레드만 멈춘다
    private static readonly TimeSpan s_retryDelay = TimeSpan.FromMilliseconds(100);

    // 이름 고르기와 쓰기를 하나씩(C++ 은 덤프 전용 스레드가 하나씩 쓴다)
    private static readonly Lock s_lock = new();

    // 남긴 파일 경로. 파일: <directory>/<프로세스 이름>_<날짜>_<시각>_pid<번호>_<reason>.dmp (C++ 과 같다). 같은 초에 같은
    // 이유로 또 남기면 덮어쓰지 않고 _2, _3 … 을 붙인다(Codex 리뷰 R4).
    public static string Write(DumpKind kind, string reason, string directory = "dumps")
    {
        ArgumentNullException.ThrowIfNull(reason);
        Directory.CreateDirectory(directory);
        using var process = Process.GetCurrentProcess();
        var safeReason = string.Concat(reason.Select(c => char.IsAsciiLetterOrDigit(c) || c == '-' ? c : '_'));
        var stem = string.Create(CultureInfo.InvariantCulture,
            $"{process.ProcessName}_{DateTime.Now:yyyyMMdd_HHmmss}_pid{process.Id}_{safeReason}");
        var dumpType = kind == DumpKind.Mini ? DumpType.WithHeap : DumpType.Full;
        lock (s_lock)
        {
            var path = FreePath(directory, stem);
            try
            {
                new DiagnosticsClient(process.Id).WriteDump(dumpType, path);
            }
            catch (ServerErrorException e) when (IsPartialCopy(e))
            {
                // 살아 있는 프로세스를 덤프하는 동안 다른 스레드가 메모리를 풀거나 바꾸면 createdump 의
                // ReadProcessMemory 가 일부만 읽고 실패할 수 있다(ERROR_PARTIAL_COPY, 테스트에서 10번에 한 번쯤).
                // 일시적인 실패라 반쯤 쓴 파일을 지우고 잠깐 뒤 한 번만 다시 쓴다. 또 실패하면 그대로 던진다.
                File.Delete(path); // 없으면 아무 일도 하지 않는다
                Thread.Sleep(s_retryDelay);
                path = FreePath(directory, stem);
                new DiagnosticsClient(process.Id).WriteDump(dumpType, path);
            }
            return path;
        }
    }

    // 런타임이 돌려준 createdump 오류 문장으로 일부 복사 실패만 고른다. 형식은 라이브러리 소스로 확인했다
    // (DiagnosticsClient.ValidateResponseMessage: 런타임 문장이 있으면 그것, 없으면 "Write dump failed - HRESULT: 0x…").
    // 0x80004005(E_FAIL)는 이 실패 때 실제로 본 HRESULT 다(일반 실패라 다른 원인도 섞일 수 있지만, 그때 드는 비용은 한 번
    // 더 써 보는 것뿐이다). 다른 예외(경로, 미지원 명령 등 다른 타입)는 다시 하지 않는다.
    private static bool IsPartialCopy(ServerErrorException e) =>
        e.Message.Contains("ERROR_PARTIAL_COPY", StringComparison.Ordinal)
        || e.Message.Contains("HRESULT: 0x80004005", StringComparison.Ordinal);

    private static string FreePath(string directory, string stem)
    {
        for (var n = 1; n <= MaxSameNameDumps; n++)
        {
            var name = n == 1 ? $"{stem}.dmp" : string.Create(CultureInfo.InvariantCulture, $"{stem}_{n}.dmp");
            var path = Path.GetFullPath(Path.Combine(directory, name));
            if (!File.Exists(path))
            {
                return path;
            }
        }
        throw new IOException($"CrashDump: too many dumps named {stem}");
    }
}
