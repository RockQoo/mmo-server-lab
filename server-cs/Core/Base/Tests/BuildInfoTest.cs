namespace Base.Tests;

public class BuildInfoTest
{
    // 벤치마크 기록에 남길 측정 조건이 빠짐없이 들어 있는지 확인한다.
    [Fact]
    public void ContainsRuntimeGcAndConfig()
    {
        var info = BuildInfo.Describe();

        Assert.Contains(".NET ", info);
        Assert.Contains("GC", info);
#if DEBUG
        Assert.Contains("Debug", info);
#else
        Assert.Contains("Release", info);
#endif
    }

    // C++의 "__cplusplus >= C++23" 확인과 같은 목적: 의도한 런타임(.NET 10 이상)에서 도는지.
    [Fact]
    public void RuntimeIsAtLeastNet10()
    {
        Assert.True(Environment.Version.Major >= 10, Environment.Version.ToString());
    }
}
