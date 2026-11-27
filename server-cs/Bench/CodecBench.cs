using System.Buffers;
using Base;
using BenchmarkDotNet.Attributes;

namespace Bench;

// 직렬화만(큐 없음). C++ Bench/MessageQueueBench.cpp 의 MessageCodec/* 와 같은 자리·같은 메시지.
//   Write  ArrayBufferWriter 하나를 비우고 다시 쓴다(C++ 은 ByteWriter 하나를 Clear 하고 다시 쓴다)
//   Read   새 객체로 읽는다. C# 코덱은 늘 새 객체를 돌려주므로 C++ ReadReuse(객체 재사용) 자리는 없다
[MemoryDiagnoser]
public class CodecBench
{
    private readonly ArrayBufferWriter<byte> _output = new(4096);
    private readonly Move _move = Move.Sample();
    private readonly Chat _chat = Chat.Sample();
    private readonly Inventory _inventory = Inventory.Sample();
    private byte[] _moveBytes = [];
    private byte[] _chatBytes = [];
    private byte[] _inventoryBytes = [];

    [GlobalSetup]
    public void Setup()
    {
        _moveBytes = Serialize(_move);
        _chatBytes = Serialize(_chat);
        _inventoryBytes = Serialize(_inventory);
    }

    [Benchmark]
    public int MoveWrite() => WriteOnce(_move);

    [Benchmark]
    public object MoveRead() => ReadOnce<Move>(_moveBytes);

    [Benchmark]
    public int ChatWrite() => WriteOnce(_chat);

    [Benchmark]
    public object ChatRead() => ReadOnce<Chat>(_chatBytes);

    [Benchmark]
    public int InventoryWrite() => WriteOnce(_inventory);

    [Benchmark]
    public object InventoryRead() => ReadOnce<Inventory>(_inventoryBytes);

    private static byte[] Serialize<T>(T value) where T : IByteCodec<T>
    {
        var output = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(output);
        T.Write(ref writer, value);
        return output.WrittenSpan.ToArray();
    }

    private static T ReadOnce<T>(byte[] bytes) where T : IByteCodec<T>
    {
        var reader = new ByteReader(bytes);
        return T.Read(ref reader);
    }

    private int WriteOnce<T>(T value) where T : IByteCodec<T>
    {
        _output.ResetWrittenCount();
        var writer = new ByteWriter(_output);
        T.Write(ref writer, value);
        return writer.Written;
    }
}
