using System.Diagnostics;

namespace Base.Tests;

// C# 은 C++ ASSERT / DEBUG_ASSERT 를 따로 만들지 않고 BCL(Base Class Library: .NET 기본 제공)을 쓴다.
//   Trace.Assert  Debug·Release 모두 검사. 실패하면 메시지 + 호출 스택을 남기고 FailFast(잡을 수 없는 종료, 0x80131623)
//   Debug.Assert  Debug 에서만 검사(Release 에서는 호출째 사라진다)
// 실패 동작 자체는 .NET 이 보장하므로 여기서는 "검사가 빌드에서 사라지지 않았는가"만 지킨다.
public class AssertTest
{
    // Trace.Assert 는 TRACE 기호가 있어야 남는다. 빠지면 Release 에서 검사가 소리 없이 사라진다.
    [Fact]
    public void TraceAssertIsCompiledIn()
    {
#if TRACE
        var evaluated = false;
        Trace.Assert(Evaluate(ref evaluated), "never fails");
        Assert.True(evaluated);
#else
        Assert.Fail("TRACE 기호가 없다: Trace.Assert 가 빌드에서 사라진다(Directory.Build.props 확인)");
#endif
    }

    // Debug.Assert 는 Debug 에서만 조건을 평가한다(C++ DEBUG_ASSERT 와 같다).
    [Fact]
    public void DebugAssertOnlyInDebug()
    {
        var evaluated = false;
        Debug.Assert(Evaluate(ref evaluated), "never fails");
#if DEBUG
        Assert.True(evaluated);
#else
        Assert.False(evaluated);
#endif
    }

    private static bool Evaluate(ref bool evaluated)
    {
        evaluated = true;
        return true;
    }
}
