using System.Buffers;
using Base;
using BenchmarkDotNet.Attributes;

namespace Bench;

// 직렬화만(큐 없음). C++ Bench/MessageQueueBench.cpp 의 MessageCodec/* 와 같은 자리·같은 메시지.
//   Write  ArrayBufferWriter 하나를 비우고 다시 쓴다(C++ 은 ByteWriter 하나를 Clear 하고 다시 쓴다)
//   Read   새 객체로 읽는다. C# 코덱은 늘 새 객체를 돌려주므로 C++ ReadReuse(객체 재사용) 자리는 없다
// 메시지의 Write/Read 를 바로 부르고 ByteWriter 도 그 자리에서 만든다. 측정 틀의 호출 모양이 숫자를 바꿨다(2026-11-28):
//   - 제네릭 함수(T.Write)로 부르면 T 가 클래스라 JIT 가 공유 코드를 만들어 인라인하지 않는다(Move 쓰기 7.3 -> 16.1ns)
//   - ByteWriter 를 돌려주는 도우미 함수로 만들면 Move 쓰기 6.4 -> 14.3ns
// 서버 코드도 같다: ByteWriter 는 쓰는 자리에서 만들고, 코덱은 struct 로 둔다(CodeGen 이 이 모양을 만든다).
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
    public int MoveWrite()
    {
        _output.ResetWrittenCount();
        var writer = new ByteWriter(_output);
        Move.Write(ref writer, _move);
        return writer.Written;
    }

    [Benchmark]
    public object MoveRead()
    {
        var reader = new ByteReader(_moveBytes);
        return Move.Read(ref reader);
    }

    [Benchmark]
    public int ChatWrite()
    {
        _output.ResetWrittenCount();
        var writer = new ByteWriter(_output);
        Chat.Write(ref writer, _chat);
        return writer.Written;
    }

    [Benchmark]
    public object ChatRead()
    {
        var reader = new ByteReader(_chatBytes);
        return Chat.Read(ref reader);
    }

    [Benchmark]
    public int InventoryWrite()
    {
        _output.ResetWrittenCount();
        var writer = new ByteWriter(_output);
        Inventory.Write(ref writer, _inventory);
        return writer.Written;
    }

    [Benchmark]
    public object InventoryRead()
    {
        var reader = new ByteReader(_inventoryBytes);
        return Inventory.Read(ref reader);
    }

    private static byte[] Serialize<T>(T value) where T : IByteCodec<T>
    {
        var output = new ArrayBufferWriter<byte>();
        var writer = new ByteWriter(output);
        T.Write(ref writer, value);
        return output.WrittenSpan.ToArray();
    }
}
