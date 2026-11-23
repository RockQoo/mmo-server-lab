using System.Buffers;

namespace Base.Tests;

// 실패 처리: 쓰기는 길이 초과·잘못된 UTF-16, 읽기는 모자란 데이터·거짓 길이·잘못된 UTF-8.
// 실패하면 표시를 남기고 이후 작업은 무시한다(C++ ByteRWTest와 같은 규칙).
public class ByteRWTest
{
    [Fact]
    public void StringLongerThanMaxCountFails()
    {
        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write(new string('a', ByteWriter.MaxCount + 1));
        writer.Write(1); // 실패 뒤 쓰기는 무시된다

        Assert.False(writer.IsValid);
        Assert.Equal(0, buffer.WrittenCount);
    }

    [Fact]
    public void StringAtMaxCountSucceeds()
    {
        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write(new string('a', ByteWriter.MaxCount));

        Assert.True(writer.IsValid);
        Assert.Equal(2 + ByteWriter.MaxCount, buffer.WrittenCount);
    }

    // 글자 수는 상한 이하여도 UTF-8 바이트 수가 넘으면 실패('가'는 3바이트).
    [Fact]
    public void StringOverMaxCountInUtf8BytesFails()
    {
        var writer = new ByteWriter(new ArrayBufferWriter<byte>());
        writer.Write(new string('가', ByteWriter.MaxCount / 3 + 1));

        Assert.False(writer.IsValid);
    }

    // 문자열은 실제 UTF-8 바이트 수만큼만 버퍼를 요청한다(글자당 3바이트로 잡으면 긴 ASCII에서 버퍼가 3배로 커진다).
    [Fact]
    public void StringRequestsExactSize()
    {
        var output = new SizeHintRecorder();
        var writer = new ByteWriter(output);
        writer.Write(new string('a', 30_000));

        Assert.True(writer.IsValid);
        Assert.Equal(2 + 30_000, output.MaxSizeHint);
    }

    // 짝이 없는 서로게이트는 U+FFFD로 바꿔 쓰지 않고 실패로 본다.
    [Fact]
    public void LoneSurrogateFails()
    {
        var writer = new ByteWriter(new ArrayBufferWriter<byte>());
        writer.Write("a\uD800b");

        Assert.False(writer.IsValid);
    }

    [Fact]
    public void ReadPastEndFails()
    {
        var reader = new ByteReader([1, 2, 3]);

        Assert.Equal(0, reader.Read<int>());
        Assert.False(reader.IsValid);
        Assert.Equal(0, reader.Read<byte>()); // 실패 뒤 읽기는 기본값
    }

    // 길이가 남은 바이트보다 크면(거짓 길이) 실패. 그 길이만큼 메모리를 잡지 않는다.
    [Fact]
    public void LengthLargerThanRemainingFails()
    {
        var reader = new ByteReader([0xff, 0xff, (byte)'a']);

        Assert.Equal("", reader.ReadString());
        Assert.False(reader.IsValid);
    }

    [Fact]
    public void InvalidUtf8Fails()
    {
        var reader = new ByteReader([2, 0, 0xc0, 0x80]); // overlong 인코딩

        Assert.Equal("", reader.ReadString());
        Assert.False(reader.IsValid);
    }

    // 0/1이 아닌 바이트도 bool로 읽으면 true(C++과 같다).
    [Fact]
    public void NonZeroByteReadsAsTrue()
    {
        var reader = new ByteReader([7]);

        Assert.True(reader.ReadBool());
        Assert.True(reader.IsValid);
    }

    // GetSpan에 요청한 크기 중 가장 큰 값을 기록하는 버퍼.
    private sealed class SizeHintRecorder : IBufferWriter<byte>
    {
        private readonly ArrayBufferWriter<byte> _inner = new();

        public int MaxSizeHint { get; private set; }

        public void Advance(int count) => _inner.Advance(count);

        public Memory<byte> GetMemory(int sizeHint = 0)
        {
            MaxSizeHint = Math.Max(MaxSizeHint, sizeHint);
            return _inner.GetMemory(sizeHint);
        }

        public Span<byte> GetSpan(int sizeHint = 0)
        {
            MaxSizeHint = Math.Max(MaxSizeHint, sizeHint);
            return _inner.GetSpan(sizeHint);
        }
    }
}
