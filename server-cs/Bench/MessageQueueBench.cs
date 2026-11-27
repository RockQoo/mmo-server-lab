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
                    ? laneGroup[l].TryPost(kind == Kind.QueueOnly ? flow.Ack : ToJob(handle, envelope))
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

    // Lane 에 넣을 일(클로저). 따로 뺀 이유: 반복 안에서 envelope 를 캡처하는 람다를 쓰면 컴파일러가 분기와 상관없이
    // 반복마다 캡처 객체를 만들어 Channel 경로에도 할당이 생긴다(Codex 리뷰 R1). 여기서는 Lane 경로에서만 만든다.
    private static Action ToJob(Action<Envelope> handle, Envelope envelope) => () => handle(envelope);

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
}
