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
    private static byte[] LoadHex(string name)
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
}
