namespace Base.Tests;

// C++ CrashDumpTest 의 수동 덤프와 같은 내용. 크래시 덤프는 런타임 설정(환경 변수)이라 테스트하지 않는다(CrashDump 주석).
public class CrashDumpTest
{
    [Fact]
    public void WritesManualDumpAndKeepsRunning()
    {
        var dir = Path.Combine(Path.GetTempPath(), "mmo-server-lab-cs-CrashDump");

        var path = CrashDump.Write(DumpKind.Mini, "manual test", dir);

        Assert.EndsWith("_manual_test.dmp", path, StringComparison.Ordinal); // 공백은 _
        Assert.True(new FileInfo(path).Length > 0);
        File.Delete(path);
    }
}
