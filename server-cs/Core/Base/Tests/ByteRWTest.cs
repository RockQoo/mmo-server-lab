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

    // 개수가 상한(uint16)을 넘으면 실패, 이후 쓰기는 무시(C++ TooLong... 과 같은 규칙).
    [Fact]
    public void ContainerLongerThanMaxCountFails()
    {
        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write<int[], IntegerArrayCodec<int>>(new int[ByteWriter.MaxCount + 1]);
        writer.Write(1);

        Assert.False(writer.IsValid);
        Assert.Equal(0, buffer.WrittenCount);
    }

    // 거짓 개수: 개수는 크게, 데이터는 조금. 실패하고 빈 컨테이너를 돌려준다(통째 복사와 원소별 둘 다).
    [Fact]
    public void ContainerCountLargerThanDataFails()
    {
        byte[] data = [3, 0, 1, 0, 0, 0];  // int 3개라 했지만 4바이트뿐

        var bulk = new ByteReader(data);
        Assert.Empty(bulk.Read<int[], IntegerArrayCodec<int>>());
        Assert.False(bulk.IsValid);

        var each = new ByteReader(data);
        Assert.Empty(each.Read<List<int>, ListCodec<int, IntegerCodec<int>>>());
        Assert.False(each.IsValid);
    }

    // 정수 배열은 원소 크기까지 따져 거짓 개수를 막는다: long 3개(24바이트)라 했는데 데이터는 그보다 적다.
    // 개수(3) 자체는 남은 바이트(8) 이하라 1바이트 기준 검사로는 통과하던 경우다.
    [Fact]
    public void IntegerArrayCountTimesSizeLargerThanDataFails()
    {
        var reader = new ByteReader([3, 0, 1, 0, 0, 0, 0, 0, 0, 0]);

        Assert.Empty(reader.Read<long[], IntegerArrayCodec<long>>());
        Assert.False(reader.IsValid);
        Assert.Equal(2, reader.ReadPos); // 개수만 읽고 멈춘다(배열을 만들지 않음)
    }

    // 값이 있다(1)고 해 놓고 값이 없으면, 0 이나 "" 가 아니라 null 이다(C++ 처럼 실패한 optional 은 비운다).
    [Fact]
    public void NullableWithMissingValueReadsAsNull()
    {
        var value = new ByteReader([1]);
        Assert.Null(value.Read<int?, NullableCodec<int, IntegerCodec<int>>>());
        Assert.False(value.IsValid);

        var reference = new ByteReader([1, 5, 0]);
        Assert.Null(reference.Read<string?, NullableRefCodec<string, StringCodec>>());
        Assert.False(reference.IsValid);
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
