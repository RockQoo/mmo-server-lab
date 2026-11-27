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
    // 남긴 파일 경로. 파일: <directory>/<프로세스 이름>_<날짜>_<시각>_pid<번호>_<reason>.dmp (C++ 과 같다)
    public static string Write(DumpKind kind, string reason, string directory = "dumps")
    {
        ArgumentNullException.ThrowIfNull(reason);
        Directory.CreateDirectory(directory);
        using var process = Process.GetCurrentProcess();
        var safeReason = string.Concat(reason.Select(c => char.IsAsciiLetterOrDigit(c) || c == '-' ? c : '_'));
        var name = string.Create(CultureInfo.InvariantCulture,
            $"{process.ProcessName}_{DateTime.Now:yyyyMMdd_HHmmss}_pid{process.Id}_{safeReason}.dmp");
        var path = Path.GetFullPath(Path.Combine(directory, name));
        new DiagnosticsClient(process.Id).WriteDump(kind == DumpKind.Mini ? DumpType.WithHeap : DumpType.Full, path);
        return path;
    }
}
