using System.Buffers;

namespace Base.Tests;

// 클래스 직렬화. C++ 의 SERIALIZE_FIELDS 자리에 C# 은 "클래스 + 코덱 struct" 쌍을 둔다(나중에 CodeGen 이 이 모양을 만든다).
// 코덱을 클래스 자신이 아닌 struct 로 따로 두는 이유: 참조 형식을 코덱으로 쓰면 JIT 가 제네릭 코드를 공유 형태로 만들어
// static abstract 호출이 인라인되지 않는다. struct 코덱은 타입마다 전용 코드가 생긴다.
public class ByteClassTest
{
    private enum Job : byte
    {
        None = 0, // enum 은 0 값을 둔다(default 로 만든 값이 유효하게, CA1008)
        Warrior = 1,
        Archer = 2,
    }

    private sealed class Item
    {
        public int Id { get; set; }
        public string Name { get; set; } = "";
    }

    private readonly struct ItemCodec : IByteCodec<Item>
    {
        public static void Write(ref ByteWriter writer, Item value)
        {
            if (value is null)
            {
                writer.Fail();
                return;
            }
            writer.Write(value.Id);
            writer.Write(value.Name);
        }

        // 객체 초기화는 적은 순서대로 평가된다(C# 규칙). 필드 순서 = 직렬화 순서.
        public static Item Read(ref ByteReader reader) => new()
        {
            Id = reader.Read<int>(),
            Name = reader.ReadString(),
        };
    }

    private sealed class Player
    {
        public int Level { get; set; }
        public Job Job { get; set; }
        public string Name { get; set; } = "";
        public List<Item> Items { get; set; } = [];
        public Dictionary<string, int> Stats { get; set; } = [];
    }

    private readonly struct PlayerCodec : IByteCodec<Player>
    {
        public static void Write(ref ByteWriter writer, Player value)
        {
            if (value is null)
            {
                writer.Fail();
                return;
            }
            writer.Write(value.Level);
            writer.WriteEnum(value.Job);
            writer.Write(value.Name);
            writer.Write<List<Item>, ListCodec<Item, ItemCodec>>(value.Items);
            writer.Write<Dictionary<string, int>, DictionaryCodec<string, int, StringCodec, IntegerCodec<int>>>(value.Stats);
        }

        public static Player Read(ref ByteReader reader) => new()
        {
            Level = reader.Read<int>(),
            Job = reader.ReadEnum<Job>(),
            Name = reader.ReadString(),
            Items = reader.Read<List<Item>, ListCodec<Item, ItemCodec>>(),
            Stats = reader.Read<Dictionary<string, int>, DictionaryCodec<string, int, StringCodec, IntegerCodec<int>>>(),
        };
    }

    [Fact]
    public void ClassesMatchGoldenFile()
    {
        var expected = ByteFormatTest.LoadHex("classes.hex");
        var player = new Player
        {
            Level = 10,
            Job = Job.Archer,
            Name = "쿠",
            Items = [new Item { Id = 1, Name = "검" }],
            Stats = new() { ["hp"] = 100, ["mp"] = 50 }, // C++ std::map 은 키 순이라 같은 순서로 넣는다
        };

        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write<Player, PlayerCodec>(player);
        Assert.True(writer.IsValid);
        Assert.Equal(expected, buffer.WrittenSpan.ToArray());

        var reader = new ByteReader(expected);
        var read = reader.Read<Player, PlayerCodec>();
        Assert.True(reader.IsValid);
        Assert.Equal(0, reader.Remaining);
        Assert.Equal((10, Job.Archer, "쿠"), (read.Level, read.Job, read.Name));
        var item = Assert.Single(read.Items);
        Assert.Equal((1, "검"), (item.Id, item.Name));
        Assert.Equal(new Dictionary<string, int> { ["hp"] = 100, ["mp"] = 50 }, read.Stats);
    }

    // null 클래스·문자열·컨테이너는 예외 대신 실패 표시(이후 쓰기 무시)
    [Fact]
    public void NullValuesFailInsteadOfThrowing()
    {
        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write<Item, ItemCodec>(null!);
        writer.Write(1);
        Assert.False(writer.IsValid);
        Assert.Equal(0, buffer.WrittenCount);

        var strings = new ByteWriter(new ArrayBufferWriter<byte>());
        strings.Write(null!);
        Assert.False(strings.IsValid);

        var lists = new ByteWriter(new ArrayBufferWriter<byte>());
        lists.Write<List<int>, ListCodec<int, IntegerCodec<int>>>(null!);
        Assert.False(lists.IsValid);
    }

    // 아무것도 쓰지 않는 값(0바이트). C++ 의 std::monostate 같은 것.
    private readonly struct NothingCodec : IByteCodec<bool>
    {
        public static void Write(ref ByteWriter writer, bool value)
        {
        }

        public static bool Read(ref ByteReader reader) => true;
    }

    // 키 1바이트 + 값 0바이트 쌍도 왕복한다(C++ map 도 한쪽이 0바이트인 쌍을 허용한다)
    [Fact]
    public void DictionaryWithZeroSizeValueRoundTrips()
    {
        var buffer = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(buffer);
        writer.Write<Dictionary<byte, bool>, DictionaryCodec<byte, bool, IntegerCodec<byte>, NothingCodec>>(new() { [7] = true });
        Assert.Equal([1, 0, 7], buffer.WrittenSpan.ToArray());

        var reader = new ByteReader(buffer.WrittenSpan);
        var map = reader.Read<Dictionary<byte, bool>, DictionaryCodec<byte, bool, IntegerCodec<byte>, NothingCodec>>();
        Assert.True(reader.IsValid);
        Assert.Equal(7, Assert.Single(map).Key);
    }

    // 같은 키가 두 번 오면 먼저 온 값을 유지한다(C++ map 읽기와 같다)
    [Fact]
    public void DuplicateKeyKeepsFirstValue()
    {
        byte[] data = [2, 0, 1, 0, (byte)'a', 1, 0, 0, 0, 1, 0, (byte)'a', 2, 0, 0, 0];
        var reader = new ByteReader(data);

        var map = reader.Read<Dictionary<string, int>, DictionaryCodec<string, int, StringCodec, IntegerCodec<int>>>();

        Assert.True(reader.IsValid);
        Assert.Equal(1, Assert.Single(map).Value);
    }
}
