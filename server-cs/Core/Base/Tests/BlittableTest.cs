using System.Buffers;

namespace Base.Tests;

// Blittable<T, TCodec>: 코덱이 필드마다 쓴 바이트가 메모리 그대로와 같은 타입만 true(C++ BlittableTest 와 같은 경우들).
// true 면 ArrayCodec·ListCodec 이 memcpy 한 번으로 쓰고 읽는다.
public class BlittableTest
{
    private enum Grade : byte
    {
        Normal = 0,
        Rare = 1,
    }

    private readonly record struct Slot(uint ItemId, uint Count, ulong Uid);

    private readonly struct SlotCodec : IByteCodec<Slot>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Slot value)
        {
            writer.Write(value.ItemId);
            writer.Write(value.Count);
            writer.Write(value.Uid);
        }

        public static Slot Read(ref ByteReader reader) =>
            new(reader.Read<uint>(), reader.Read<uint>(), reader.Read<ulong>());
    }

    // 쓰는 순서가 메모리 순서와 다르다 → 통째 복사하면 바이트가 달라진다
    private readonly struct ReorderedSlotCodec : IByteCodec<Slot>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Slot value)
        {
            writer.Write(value.Count);
            writer.Write(value.ItemId);
            writer.Write(value.Uid);
        }

        public static Slot Read(ref ByteReader reader)
        {
            var count = reader.Read<uint>();
            var itemId = reader.Read<uint>();
            return new(itemId, count, reader.Read<ulong>());
        }
    }

    // 필드 하나를 안 쓴다
    private readonly struct MissingSlotCodec : IByteCodec<Slot>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Slot value)
        {
            writer.Write(value.ItemId);
            writer.Write(value.Count);
        }

        public static Slot Read(ref ByteReader reader) => new(reader.Read<uint>(), reader.Read<uint>(), 0);
    }

    private readonly record struct Padded(byte Kind, uint Value); // byte 뒤에 빈틈 3바이트

    private readonly struct PaddedCodec : IByteCodec<Padded>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Padded value)
        {
            writer.Write(value.Kind);
            writer.Write(value.Value);
        }

        public static Padded Read(ref ByteReader reader) => new(reader.Read<byte>(), reader.Read<uint>());
    }

    private readonly record struct Flagged(uint Id, bool Locked, byte Reserved1, ushort Reserved2);

    private readonly struct FlaggedCodec : IByteCodec<Flagged>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Flagged value)
        {
            writer.Write(value.Id);
            writer.Write(value.Locked);
            writer.Write(value.Reserved1);
            writer.Write(value.Reserved2);
        }

        public static Flagged Read(ref ByteReader reader) =>
            new(reader.Read<uint>(), reader.ReadBool(), reader.Read<byte>(), reader.Read<ushort>());
    }

    private readonly record struct Unit(float X, float Y, float Z, Grade Grade, byte Level, ushort Hp);

    private readonly struct UnitCodec : IByteCodec<Unit>
    {
        public static bool IsPlainCopy => true;

        public static void Write(ref ByteWriter writer, Unit value)
        {
            writer.Write(value.X);
            writer.Write(value.Y);
            writer.Write(value.Z);
            writer.WriteEnum(value.Grade);
            writer.Write(value.Level);
            writer.Write(value.Hp);
        }

        public static Unit Read(ref ByteReader reader) =>
            new(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle(), reader.ReadEnum<Grade>(),
                reader.Read<byte>(), reader.Read<ushort>());
    }

    [Fact]
    public void DecidesBlittableTypes()
    {
        Assert.True(Blittable<int, IntegerCodec<int>>.IsBlittable);
        Assert.True(Blittable<double, DoubleCodec>.IsBlittable);
        Assert.True(Blittable<Slot, SlotCodec>.IsBlittable);
        Assert.True(Blittable<Unit, UnitCodec>.IsBlittable); // float, enum 도 된다

        Assert.False(Blittable<bool, BoolCodec>.IsBlittable);
        Assert.False(Blittable<Slot, ReorderedSlotCodec>.IsBlittable);
        Assert.False(Blittable<Slot, MissingSlotCodec>.IsBlittable);
        Assert.False(Blittable<Padded, PaddedCodec>.IsBlittable);
        Assert.False(Blittable<Flagged, FlaggedCodec>.IsBlittable);
        Assert.False(Blittable<string, StringCodec>.IsBlittable);
    }

    // 음수는 실패로 치는 코덱(IsPlainCopy 를 밝히지 않음). 통째 복사로 바꾸면 이 검사를 건너뛴다(리뷰 R1)
    private readonly struct NonNegativeCodec : IByteCodec<int>
    {
        public static void Write(ref ByteWriter writer, int value)
        {
            if (value < 0)
            {
                writer.Fail();
                return;
            }
            writer.Write(value);
        }

        public static int Read(ref ByteReader reader) => reader.Read<int>();
    }

    // 범위 밖 값(0~100 밖)이면 읽을 때 예외를 던지는 코덱. 아무 바이트로 써 보면 타입 초기화가 영원히 실패한다(리뷰 R2)
    private readonly struct PercentCodec : IByteCodec<int>
    {
        public static void Write(ref ByteWriter writer, int value) => writer.Write(value);

        public static int Read(ref ByteReader reader)
        {
            var value = reader.Read<int>();
            ArgumentOutOfRangeException.ThrowIfGreaterThan(value, 100);
            return value;
        }
    }

    [Fact]
    public void CodecWithoutPlainCopyKeepsItsOwnRules()
    {
        Assert.False(Blittable<int, NonNegativeCodec>.IsBlittable);
        Assert.False(Blittable<int, PercentCodec>.IsBlittable); // 판별이 예외를 던지지 않는다

        var output = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(output);
        writer.Write<int[], ArrayCodec<int, NonNegativeCodec>>([1, -2]);
        Assert.False(writer.IsValid);

        var percents = new ArrayBufferWriter<byte>();
        var percentWriter = new ByteWriter(percents);
        percentWriter.Write<int[], ArrayCodec<int, PercentCodec>>([0, 55, 100]);
        var reader = new ByteReader(percents.WrittenSpan);
        Assert.Equal([0, 55, 100], reader.Read<int[], ArrayCodec<int, PercentCodec>>());
        Assert.True(reader.IsValid);
    }

    // 한 번에 쓴 바이트 = 필드마다 쓴 바이트(형식이 바뀌지 않는다. C++ 과 주고받는 근거)
    [Fact]
    public void BlockWriteMatchesFieldByFieldBytes()
    {
        List<Slot> slots = [new(1, 2, 3), new(40, 50, 60)];
        Unit[] units = [new(1.5F, -2.0F, 3.25F, Grade.Rare, 7, 900), default];

        var block = new ArrayBufferWriter<byte>();
        var blockWriter = new ByteWriter(block);
        blockWriter.Write<List<Slot>, ListCodec<Slot, SlotCodec>>(slots);
        blockWriter.Write<Unit[], ArrayCodec<Unit, UnitCodec>>(units);

        var fields = new ArrayBufferWriter<byte>();
        var fieldWriter = new ByteWriter(fields);
        fieldWriter.Write((ushort)slots.Count);
        foreach (var slot in slots)
        {
            SlotCodec.Write(ref fieldWriter, slot);
        }
        fieldWriter.Write((ushort)units.Length);
        foreach (var unit in units)
        {
            UnitCodec.Write(ref fieldWriter, unit);
        }

        Assert.True(blockWriter.IsValid);
        Assert.Equal(fields.WrittenSpan.ToArray(), block.WrittenSpan.ToArray());
    }

    [Fact]
    public void BlockReadRoundTrips()
    {
        var output = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(output);
        writer.Write<List<Slot>, ListCodec<Slot, SlotCodec>>([new(1, 2, 3), new(40, 50, 60)]);
        writer.Write<Unit[], ArrayCodec<Unit, UnitCodec>>([new(1, 2, 9, Grade.Rare, 3, 4)]);

        var reader = new ByteReader(output.WrittenSpan);
        var slots = reader.Read<List<Slot>, ListCodec<Slot, SlotCodec>>();
        var units = reader.Read<Unit[], ArrayCodec<Unit, UnitCodec>>();

        Assert.True(reader.IsValid);
        Assert.Equal([new(1, 2, 3), new(40, 50, 60)], slots);
        Assert.Equal(9F, Assert.Single(units).Z);
    }

    // 개수만큼의 바이트가 없으면 만들기 전에 실패, 빈 List
    [Fact]
    public void ShortBufferFails()
    {
        var output = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(output);
        writer.Write<List<Slot>, ListCodec<Slot, SlotCodec>>([default, default, default, default]);
        var data = output.WrittenSpan;

        var reader = new ByteReader(data[..^1]); // 마지막 1바이트가 없다
        var read = reader.Read<List<Slot>, ListCodec<Slot, SlotCodec>>();

        Assert.False(reader.IsValid);
        Assert.Empty(read);
    }
}
