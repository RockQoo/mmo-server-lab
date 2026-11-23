using System.Buffers;
using System.Globalization;

namespace Base.Tests;

// C++(server-cpp/Core/Base/Tests/ByteFormatTest.cpp)과 같은 정답 파일(testdata/bytes/)을 쓴다.
// 한쪽 언어의 형식이 바뀌면 양쪽 테스트가 같이 깨진다. 파일은 Base.Tests.csproj가 출력 폴더로 복사한다.
public class ByteFormatTest
{
    private enum Color : byte
    {
        Red = 1,
        Green = 2,
        Blue = 3,
    }

    // "fe ff # 주석" 형식의 줄을 바이트로 바꾼다.
    internal static byte[] LoadHex(string name)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "testdata", "bytes", name);
        return File.ReadLines(path)
            .Select(line => line.Split('#')[0])
            .SelectMany(line => line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
            .Select(token => byte.Parse(token, NumberStyles.HexNumber))
            .ToArray();
    }

    [Fact]
    public void BasicMatchesGoldenFile()
    {
        var expected = LoadHex("basic.hex");
        Assert.NotEmpty(expected);

        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write(-2);
        writer.Write((ushort)0x1234);
        writer.Write(true);
        writer.Write(false);
        writer.Write(1.5f);
        writer.Write(-2.25);
        writer.WriteEnum(Color.Blue);
        writer.Write(0x0102030405060708L);
        writer.Write("가a");
        writer.Write("");
        Assert.True(writer.IsValid);
        Assert.Equal(expected, buffer.WrittenSpan.ToArray());

        // 정답 바이트를 읽으면 같은 값이 나와야 한다.
        var reader = new ByteReader(expected);
        Assert.Equal(-2, reader.Read<int>());
        Assert.Equal((ushort)0x1234, reader.Read<ushort>());
        Assert.True(reader.ReadBool());
        Assert.False(reader.ReadBool());
        Assert.Equal(1.5f, reader.ReadSingle());
        Assert.Equal(-2.25, reader.ReadDouble());
        Assert.Equal(Color.Blue, reader.ReadEnum<Color>());
        Assert.Equal(0x0102030405060708L, reader.Read<long>());
        Assert.Equal("가a", reader.ReadString());
        Assert.Equal("", reader.ReadString());
        Assert.True(reader.IsValid);
        Assert.Equal(0, reader.Remaining);
    }

    // C++ vector / optional 과 같은 바이트. 코덱을 조합해 중첩(List<short?>, byte[][])도 쓴다.
    [Fact]
    public void ContainersMatchGoldenFile()
    {
        var expected = LoadHex("containers.hex");
        Assert.NotEmpty(expected);

        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write<int[], IntegerArrayCodec<int>>([1, 2, 3]);
        writer.Write<int[], ArrayCodec<int, IntegerCodec<int>>>([]);
        writer.Write<List<string>, ListCodec<string, StringCodec>>(["a", "가"]);
        writer.Write<int?, NullableCodec<int, IntegerCodec<int>>>(7);
        writer.Write<int?, NullableCodec<int, IntegerCodec<int>>>(null);
        writer.Write<string?, NullableRefCodec<string, StringCodec>>("x");
        writer.Write<List<short?>, ListCodec<short?, NullableCodec<short, IntegerCodec<short>>>>([5, null]);
        writer.Write<byte[][], ArrayCodec<byte[], IntegerArrayCodec<byte>>>([[1, 2], []]);
        Assert.True(writer.IsValid);
        Assert.Equal(expected, buffer.WrittenSpan.ToArray());

        var reader = new ByteReader(expected);
        Assert.Equal([1, 2, 3], reader.Read<int[], IntegerArrayCodec<int>>());
        Assert.Empty(reader.Read<int[], ArrayCodec<int, IntegerCodec<int>>>());
        Assert.Equal(["a", "가"], reader.Read<List<string>, ListCodec<string, StringCodec>>());
        Assert.Equal(7, reader.Read<int?, NullableCodec<int, IntegerCodec<int>>>());
        Assert.Null(reader.Read<int?, NullableCodec<int, IntegerCodec<int>>>());
        Assert.Equal("x", reader.Read<string?, NullableRefCodec<string, StringCodec>>());
        Assert.Equal([5, null], reader.Read<List<short?>, ListCodec<short?, NullableCodec<short, IntegerCodec<short>>>>());
        var nested = reader.Read<byte[][], ArrayCodec<byte[], IntegerArrayCodec<byte>>>();
        Assert.Equal(2, nested.Length);
        Assert.Equal([1, 2], nested[0]);
        Assert.Empty(nested[1]);
        Assert.True(reader.IsValid);
        Assert.Equal(0, reader.Remaining);
    }
}
