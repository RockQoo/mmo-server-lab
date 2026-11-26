using System.Buffers;

namespace Base.Tests;

// C++ RuidTest / StrongIdTest 와 같은 내용. 범위 밖 값의 Trace.Assert(FailFast)는 프로세스를 끝내 xUnit 으로 확인할 수 없어
// C++ death test 가 맡는다(AssertTest 주석과 같은 이유).
public class RuidTest
{
    [Fact]
    public void LayoutLimits()
    {
        Assert.Equal(63, Ruid.TimestampBits + Ruid.RealmBits + Ruid.NodeBits + Ruid.SequenceBits);
        Assert.Equal((1L << 41) - 1, Ruid.MaxTimestamp);
        Assert.Equal(127, Ruid.MaxRealm);
        Assert.Equal(63, Ruid.MaxNode);
        Assert.Equal(511, Ruid.MaxSequence);
    }

    // 각 칸의 최솟값·최댓값을 넣고 그대로 꺼내진다(칸끼리 비트가 겹치지 않는다).
    [Fact]
    public void RoundTripsEachFieldAtLimits()
    {
        var min = Ruid.Make(0, Ruid.MinRealm, 0, 0);
        Assert.Equal(0, min.Timestamp);
        Assert.Equal(Ruid.MinRealm, min.Realm);
        Assert.Equal(0, min.Node);
        Assert.Equal(0, min.Sequence);

        var max = Ruid.Make(Ruid.MaxTimestamp, Ruid.MaxRealm, Ruid.MaxNode, Ruid.MaxSequence);
        Assert.Equal(Ruid.MaxTimestamp, max.Timestamp);
        Assert.Equal(Ruid.MaxRealm, max.Realm);
        Assert.Equal(Ruid.MaxNode, max.Node);
        Assert.Equal(Ruid.MaxSequence, max.Sequence);

        var mixed = Ruid.Make(123_456_789, 42, 17, 300);
        Assert.Equal(123_456_789, mixed.Timestamp);
        Assert.Equal(42, mixed.Realm);
        Assert.Equal(17, mixed.Node);
        Assert.Equal(300, mixed.Sequence);
    }

    // C++ 과 같은 칸 값이면 같은 정수다(두 서버가 같은 DB, 같은 클라이언트를 쓴다).
    // 값은 C++ 식 (123456789 << 22) | (42 << 15) | (17 << 9) | 300 을 그대로 계산한 것이다.
    [Fact]
    public void SameValueAsCpp()
    {
        Assert.Equal((123_456_789L << 22) | (42L << 15) | (17L << 9) | 300L, Ruid.Make(123_456_789, 42, 17, 300).Value);
    }

    // 시각이 맨 위라 1ms 라도 늦게 만든 ID 가 항상 크다(다른 칸이 모두 최댓값이어도).
    [Fact]
    public void LaterTimestampIsAlwaysGreater()
    {
        var earlier = Ruid.Make(1000, Ruid.MaxRealm, Ruid.MaxNode, Ruid.MaxSequence);
        var later = Ruid.Make(1001, Ruid.MinRealm, 0, 0);
        Assert.True(earlier < later);
        Assert.True(earlier.CompareTo(later) < 0);
    }

    [Fact]
    public void AlwaysPositive()
    {
        Assert.True(Ruid.Make(Ruid.MaxTimestamp, Ruid.MaxRealm, Ruid.MaxNode, Ruid.MaxSequence).Value > 0);
    }

    // Realm 0이 예약이라 가장 작은 ID 도 default(0, 없음)가 아니다.
    [Fact]
    public void DefaultIsNeverIssued()
    {
        Assert.Equal(0, default(Ruid).Value);
        Assert.NotEqual(default, Ruid.Make(0, Ruid.MinRealm, 0, 0));
    }

    [Fact]
    public void UnixMsAddsEpoch()
    {
        Assert.Equal(Ruid.EpochUnixMs + 1500, Ruid.Make(1500, 1, 0, 0).UnixMs);
    }

    // 같은 값이면 같고, 해시 키로 쓸 수 있다(record struct 라 박싱 없이 비교한다).
    [Fact]
    public void EqualityAndHashKey()
    {
        var ids = new HashSet<Ruid> { new(1), new(2), new(1) };
        Assert.Equal(2, ids.Count);
        Assert.Equal(new Ruid(7), new Ruid(7));
        Assert.Equal("42", new Ruid(42).ToString());
    }

    // 직렬화하면 long 하나와 같은 바이트다(C++ StrongId 와 같은 형식).
    [Fact]
    public void SerializesAsPlainLong()
    {
        var idBuffer = new ArrayBufferWriter<byte>();
        var idWriter = new ByteWriter(idBuffer);
        idWriter.Write<Ruid, RuidCodec>(new Ruid(0x0102_0304_0506_0708));
        var longBuffer = new ArrayBufferWriter<byte>();
        var longWriter = new ByteWriter(longBuffer);
        longWriter.Write(0x0102_0304_0506_0708L);
        Assert.True(idWriter.IsValid);
        Assert.Equal(longBuffer.WrittenSpan.ToArray(), idBuffer.WrittenSpan.ToArray());

        var reader = new ByteReader(idBuffer.WrittenSpan);
        Assert.Equal(new Ruid(0x0102_0304_0506_0708), reader.Read<Ruid, RuidCodec>());
        Assert.True(reader.IsValid);
    }
}
