using System.Buffers;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using Base;
using BenchmarkDotNet.Attributes;
using Messaging;

namespace Bench;

// 메시지 큐에 객체를 넣을까, 직렬화한 바이트를 넣을까(C++ Bench/MessageQueueBench.cpp 와 같은 틀·횟수·조합).
// 결과는 docs/decisions/002-message-queue-object-vs-bytes.md.
//
// 모양: 생산자 N개(ThreadTeam 전용 스레드)가 소비자 M개에 메시지를 나눠 넣는다. 메시지마다 키(플레이어 ID처럼 계속
// 바뀌는 값)로 소비자를 고른다(LaneGroup.IndexFor, C++ 과 같은 계산). Mean = 전체 걸린 시간 / 전체 메시지 수.
// 흐름 제어: (생산자, 소비자) 쌍마다 처리 안 된 메시지가 Window 개를 넘으면 생산자가 기다린다. 쌍마다 카운터를 따로 둬서
// 카운터를 쓰는 스레드가 늘 하나다(측정 틀이 경합을 만들지 않게).
//
// 큐 두 가지(C#이 기본 제공하는 것이 기준)
//   Channel  BCL Channel<Envelope>(SingleReader). 소비자마다 하나, 스레드 풀에서 도는 async 루프가 꺼내 처리한다.
//            메시지 타입을 그대로 넣는다(클로저 없음)
//   Lane     우리 Messaging.Lane(Channel<Action> 위). 메시지마다 클로저(Action)를 하나 할당한다
//
// 보내는 방식(생산자는 원본 메시지를 갖고 있고 그 내용을 보낸다)
//   QueueOnly  빈 메시지. 큐 자체의 비용
//   New        새 객체로 복사해 참조를 넘긴다. 해제는 GC. (string 은 불변이라 참조만 복사한다: C#에서 자연스러운 방식)
//   Bytes      ByteWriter 로 ArrayBufferWriter 에 쓰고 ArrayPool 배열에 복사해 넘긴다. 소비자가 ByteReader 로 새 객체에
//              읽어 처리하고 배열을 반납한다(C++ Bytes 와 같은 순서)
//   (DefaultObjectPool 은 new 보다 느려서 넣지 않았다. docs/journal/objectpool.html)
// 처리(Sum)는 모든 필드를 읽어 합친다.
[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 3, iterationCount: 5)]
public class MessageQueueBench
{
    private const int TotalMessages = 1 << 20; // C++ 과 같은 수(1, 2, 4, 8 로 나누어떨어진다)
    private const long Window = 1024;
    private const int CheckEvery = 64;

    private const int QueueOnlyLength = -2; // Envelope.Length: 빈 메시지
    private const int ObjectLength = -1;    // Envelope.Length: 객체. 0 이상이면 바이트 수

    private ThreadTeam? _team;
    private Flow[] _flows = [];
    private LaneGroup? _lanes;
    private Channel<Envelope>[] _channels = [];
    private Task[] _consumers = [];
    private Action<Envelope> _handle = static _ => { };
    private int _producers;
    private int _lanesCount;

    // 생산자:소비자. 물리 코어 8개라 8:4(12스레드)는 코어가 모자란 경우다(C++ 과 같은 조합)
    [Params("1:1", "4:1", "2:2", "4:2", "4:4", "8:4")]
    public string Shape { get; set; } = "1:1";

    [GlobalSetup]
    public void Setup()
    {
        var parts = Shape.Split(':');
        _producers = int.Parse(parts[0], System.Globalization.CultureInfo.InvariantCulture);
        _lanesCount = int.Parse(parts[1], System.Globalization.CultureInfo.InvariantCulture);
        _team = new ThreadTeam(_producers);
        _flows = new Flow[_producers * _lanesCount];
        for (var f = 0; f < _flows.Length; ++f)
        {
            _flows[f] = new Flow();
        }
        _lanes = new LaneGroup(_lanesCount);
        _channels = new Channel<Envelope>[_lanesCount];
        _consumers = new Task[_lanesCount];
        for (var l = 0; l < _lanesCount; ++l)
        {
            var channel = Channel.CreateUnbounded<Envelope>(new UnboundedChannelOptions { SingleReader = true });
            _channels[l] = channel;
            _consumers[l] = Task.Run(() => ConsumeAsync(channel.Reader));
        }
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        foreach (var channel in _channels)
        {
            channel.Writer.TryComplete();
        }
        Task.WaitAll(_consumers);
        _lanes?.DisposeAsync().AsTask().Wait();
        _team?.Dispose();
    }

    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelQueueOnly() => Run<Move>(useLane: false, Kind.QueueOnly);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneQueueOnly() => Run<Move>(useLane: true, Kind.QueueOnly);

    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelMoveNew() => Run<Move>(useLane: false, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelMoveBytes() => Run<Move>(useLane: false, Kind.Bytes);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneMoveNew() => Run<Move>(useLane: true, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneMoveBytes() => Run<Move>(useLane: true, Kind.Bytes);

    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelChatNew() => Run<Chat>(useLane: false, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelChatBytes() => Run<Chat>(useLane: false, Kind.Bytes);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneChatNew() => Run<Chat>(useLane: true, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneChatBytes() => Run<Chat>(useLane: true, Kind.Bytes);

    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelInventoryNew() => Run<Inventory>(useLane: false, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void ChannelInventoryBytes() => Run<Inventory>(useLane: false, Kind.Bytes);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneInventoryNew() => Run<Inventory>(useLane: true, Kind.New);
    [Benchmark(OperationsPerInvoke = TotalMessages)]
    public void LaneInventoryBytes() => Run<Inventory>(useLane: true, Kind.Bytes);

    private void Run<T>(bool useLane, Kind kind) where T : class, IBenchMessage<T>
    {
        _handle = Handle<T>;
        var flows = _flows;
        var lanes = _lanesCount;
        var perProducer = TotalMessages / _producers;
        var laneGroup = _lanes!;
        var channels = _channels;
        var handle = _handle;

        _team!.RunEach(producer =>
        {
            var source = T.Sample();
            var writer = new ArrayBufferWriter<byte>();
            var sent = new long[lanes];
            var first = producer * lanes;
            for (var l = 0; l < lanes; ++l)
            {
                Volatile.Write(ref flows[first + l].Processed, 0); // 이전 측정은 모두 처리됐다(아래 기다림)
            }
            var keyBase = (long)producer << 32;
            for (var i = 0; i < perProducer; ++i)
            {
                var l = LaneGroup.IndexFor(keyBase + i, lanes);
                var flow = flows[first + l];
                if (sent[l] % CheckEvery == 0)
                {
                    while (sent[l] - Volatile.Read(ref flow.Processed) >= Window)
                    {
                        Thread.Yield();
                    }
                }
                ++sent[l];
                var envelope = Make(kind, flow, source, writer);
                var ok = useLane
                    ? laneGroup[l].TryPost(kind == Kind.QueueOnly ? flow.Ack : () => handle(envelope))
                    : channels[l].Writer.TryWrite(envelope);
                if (!ok)
                {
                    Environment.FailFast("bench queue closed");
                }
            }
            for (var l = 0; l < lanes; ++l)
            {
                Volatile.Write(ref flows[first + l].Sent, sent[l]);
            }
        });

        // 모든 쌍이 보낸 만큼 처리될 때까지 기다린다(Channel 에는 C++ 처럼 표시 일을 넣을 수 없어 카운터로 본다).
        foreach (var flow in flows)
        {
            while (Volatile.Read(ref flow.Processed) < Volatile.Read(ref flow.Sent))
            {
                Thread.Yield();
            }
        }
    }

    private static Envelope Make<T>(Kind kind, Flow flow, T source, ArrayBufferWriter<byte> output)
        where T : class, IBenchMessage<T>
    {
        switch (kind)
        {
            case Kind.QueueOnly:
                return new Envelope(flow, null, QueueOnlyLength);
            case Kind.New:
                return new Envelope(flow, source.Clone(), ObjectLength);
            default:
                output.ResetWrittenCount();
                var writer = new ByteWriter(output);
                T.Write(ref writer, source);
                if (!writer.IsValid)
                {
                    Environment.FailFast("bench write failed");
                }
                var length = output.WrittenCount;
                var buffer = ArrayPool<byte>.Shared.Rent(length);
                output.WrittenSpan.CopyTo(buffer);
                return new Envelope(flow, buffer, length);
        }
    }

    private static void Handle<T>(Envelope envelope) where T : class, IBenchMessage<T>
    {
        var flow = envelope.Flow;
        switch (envelope.Length)
        {
            case QueueOnlyLength:
                flow.Consume(1);
                break;
            case ObjectLength:
                flow.Consume(((T)envelope.Payload!).Sum());
                break;
            default:
                var buffer = (byte[])envelope.Payload!;
                var reader = new ByteReader(buffer.AsSpan(0, envelope.Length));
                var message = T.Read(ref reader);
                if (!reader.IsValid)
                {
                    Environment.FailFast("bench read failed");
                }
                ArrayPool<byte>.Shared.Return(buffer);
                flow.Consume(message.Sum());
                break;
        }
    }

    private async Task ConsumeAsync(ChannelReader<Envelope> reader)
    {
        // C# Lane 의 루프와 같다: 쌓인 만큼 한꺼번에 꺼내 처리하고, 비면 스레드 없이 기다린다.
        while (await reader.WaitToReadAsync().ConfigureAwait(false))
        {
            while (reader.TryRead(out var envelope))
            {
                _handle(envelope);
            }
        }
    }

    private enum Kind
    {
        QueueOnly,
        New,
        Bytes,
    }

    // 큐에 넣는 한 칸. Length 로 내용 종류를 나눈다(QueueOnlyLength, ObjectLength, 0 이상 = 바이트 수).
    private readonly record struct Envelope(Flow Flow, object? Payload, int Length);

    // (생산자, 소비자) 쌍 하나의 흐름 제어 + 합계. Processed·Checksum 은 소비자만, Sent 는 생산자만 쓴다.
    // 객체들이 힙에 붙어 놓이므로 이웃 쌍과 같은 캐시 줄에 놓이지 않게 필드를 64B 이상 띄운다.
    [StructLayout(LayoutKind.Explicit, Size = 192)]
    private sealed class Flow
    {
        [FieldOffset(64)]
        public long Processed;
        [FieldOffset(72)]
        public long Checksum;
        [FieldOffset(136)]
        public long Sent;

        // Lane QueueOnly 가 넣는 일. 미리 만들어 둬서 메시지마다 할당하지 않는다(큐 자체의 비용만 보려고)
        [FieldOffset(0)]
        public readonly Action Ack;

        public Flow() => Ack = () => Consume(1);

        public void Consume(long sum)
        {
            Checksum = unchecked(Checksum + sum);
            Volatile.Write(ref Processed, Processed + 1);
        }
    }

    // ---- 메시지(C++ 과 같은 모양·직렬화 크기)

    private interface IBenchMessage<TSelf> : IByteCodec<TSelf> where TSelf : class, IBenchMessage<TSelf>
    {
        static abstract TSelf Sample();
        TSelf Clone();
        long Sum();
    }

    // 이동 명령: id + 좌표 3개 + 플래그, 24B
    private sealed class Move : IBenchMessage<Move>
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
    private sealed class Chat : IBenchMessage<Chat>
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

    private readonly record struct Slot(uint ItemId, uint Count, ulong Uid);

    private readonly struct SlotCodec : IByteCodec<Slot>
    {
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
    private sealed class Inventory : IBenchMessage<Inventory>
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
}
