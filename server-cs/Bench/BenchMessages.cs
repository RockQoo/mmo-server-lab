using Base;

namespace Bench;

// 벤치마크 메시지 세 가지(C++ Bench/MessageQueueBench.cpp 의 Move, Chat, Inventory 와 같은 값).
// MessageQueueBench(큐에 넣기)와 CodecBench(직렬화만)가 같이 쓴다.

internal interface IBenchMessage<TSelf> : IByteCodec<TSelf> where TSelf : class, IBenchMessage<TSelf>
{
    static abstract TSelf Sample();
    TSelf Clone();
    long Sum();
}

// 이동 명령: id + 좌표 3개 + 플래그, 24B
internal sealed class Move : IBenchMessage<Move>
{
    public ulong Id { get; init; }
    public float X { get; init; }
    public float Y { get; init; }
    public float Z { get; init; }
    public uint Flags { get; init; }

    public static Move Sample() => new() { Id = 1, X = 1, Y = 2, Z = 3, Flags = 7 };
    public Move Clone() => new() { Id = Id, X = X, Y = Y, Z = Z, Flags = Flags };
    public long Sum() => unchecked((long)Id + (long)(X + Y + Z) + Flags);

    public static void Write(ref ByteWriter writer, Move value)
    {
        writer.Write(value.Id);
        writer.Write(value.X);
        writer.Write(value.Y);
        writer.Write(value.Z);
        writer.Write(value.Flags);
    }

    public static Move Read(ref ByteReader reader) => new()
    {
        Id = reader.Read<ulong>(),
        X = reader.ReadSingle(),
        Y = reader.ReadSingle(),
        Z = reader.ReadSingle(),
        Flags = reader.Read<uint>(),
    };
}

// 채팅: id + 문자열 96자, 106B
internal sealed class Chat : IBenchMessage<Chat>
{
    public ulong Id { get; init; }
    public string Text { get; init; } = "";

    public static Chat Sample() => new() { Id = 1, Text = new string('a', 96) };
    public Chat Clone() => new() { Id = Id, Text = Text };

    public long Sum()
    {
        var sum = (long)Id;
        foreach (var c in Text)
        {
            sum += c;
        }
        return sum;
    }

    public static void Write(ref ByteWriter writer, Chat value)
    {
        writer.Write(value.Id);
        writer.Write(value.Text);
    }

    public static Chat Read(ref ByteReader reader) => new()
    {
        Id = reader.Read<ulong>(),
        Text = reader.ReadString(),
    };
}

internal readonly record struct Slot(uint ItemId, uint Count, ulong Uid);

internal readonly struct SlotCodec : IByteCodec<Slot>
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

// 인벤토리 일부: 주인 + 칸 60개(칸마다 16B), 970B
internal sealed class Inventory : IBenchMessage<Inventory>
{
    public ulong Owner { get; init; }
    public List<Slot> Slots { get; init; } = [];

    public static Inventory Sample()
    {
        var slots = new List<Slot>(60);
        for (var i = 0u; i < 60; ++i)
        {
            slots.Add(new Slot(i, i + 1, (ulong)i * 1000));
        }
        return new Inventory { Owner = 1, Slots = slots };
    }

    public Inventory Clone() => new() { Owner = Owner, Slots = [.. Slots] };

    public long Sum()
    {
        var sum = (long)Owner;
        foreach (var slot in Slots)
        {
            sum = unchecked(sum + slot.ItemId + slot.Count + (long)slot.Uid);
        }
        return sum;
    }

    public static void Write(ref ByteWriter writer, Inventory value)
    {
        writer.Write(value.Owner);
        writer.Write<List<Slot>, ListCodec<Slot, SlotCodec>>(value.Slots);
    }

    public static Inventory Read(ref ByteReader reader) => new()
    {
        Owner = reader.Read<ulong>(),
        Slots = reader.Read<List<Slot>, ListCodec<Slot, SlotCodec>>(),
    };
}
