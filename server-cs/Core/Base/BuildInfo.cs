using System.Runtime;
using System.Runtime.InteropServices;

namespace Base;

// 이 바이너리를 실행한 런타임, CPU, GC 모드, 빌드 구성.
// 벤치마크 결과마다 측정 조건으로 함께 남긴다(C++ Base::BuildInfo()와 같은 역할).
// C#에는 자유 함수가 없어서 정적 클래스 + Describe()로 둔다.
public static class BuildInfo
{
    public static string Describe()
    {
        // C++은 컴파일러가 결과를 정하지만, C#은 실행하는 런타임과 GC 모드가 성능을 크게 바꾼다.
        // 그래서 컴파일러 대신 런타임 버전과 GC 모드를 남긴다.
        var gc = GCSettings.IsServerGC ? "ServerGC" : "WorkstationGC";

#if DEBUG
        const string config = "Debug";
#else
        const string config = "Release";
#endif

        return $"{RuntimeInformation.FrameworkDescription}, {RuntimeInformation.ProcessArchitecture}, {gc}, {config}";
    }
}
