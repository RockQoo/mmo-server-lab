using Base;

namespace Messaging.Tests;

// C++ LaneGroupTest 와 같은 내용(스레드 이름 테스트는 C# 에 이름이 없어서 없다).
public class LaneGroupTest
{
    [Fact]
    public async Task SameKeyAlwaysSameLane()
    {
        await using var group = new LaneGroup(8);
        for (var key = 0L; key < 1000; key++)
        {
            Assert.Same(group.LaneFor(key), group.LaneFor(key));
        }
    }

    // 섞기 결과가 C++ 과 같다(두 서버가 같은 키에 같은 번호의 Lane 을 고른다). 값은 C++ IndexForMatchesKnownValues 와 같다.
    [Fact]
    public void IndexForMatchesCpp()
    {
        Assert.Equal(0, LaneGroup.IndexFor(0, 8));
        Assert.Equal(5, LaneGroup.IndexFor(1, 8)); // Mix(1) = 0x5692161D100B05E5
        Assert.Equal(2, LaneGroup.IndexFor(2, 8)); // Mix(2) = 0xDBD238973A2B148A
        Assert.Equal(0, LaneGroup.IndexFor(3, 8)); // Mix(3) = 0x1E535EEDE31428F0
        Assert.Equal(6, LaneGroup.IndexFor(12345, 7));
    }

    // 드물게 만든 Ruid(순번이 늘 0)도 고르게 퍼진다. 같은 ID 를 key % 8 로 나누면 전부 Lane 0이다(섞는 이유).
    [Fact]
    public void SpreadsSparseRuids()
    {
        const int LaneCount = 8;
        const int IdCount = 1000;
        var mixed = new int[LaneCount];
        var plainModulo = new int[LaneCount];
        for (var i = 0; i < IdCount; i++)
        {
            var id = Ruid.Make(1000 + (long)i * 7919, 1, 3, 0); // 서로 다른 ms, 순번 0
            mixed[LaneGroup.IndexFor(id.Value, LaneCount)]++;
            plainModulo[id.Value % LaneCount]++;
        }

        Assert.Equal(IdCount, plainModulo[0]);
        const int Mean = IdCount / LaneCount;
        Assert.All(mixed, count => Assert.InRange(count, Mean * 6 / 10, Mean * 14 / 10));
    }

    [Fact]
    public async Task IndexerGivesDistinctLanes()
    {
        await using var group = new LaneGroup(4);
        Assert.Equal(4, group.Count);
        Assert.NotSame(group[0], group[1]);
        Assert.Throws<IndexOutOfRangeException>(() => group[4]);
    }

    // 같은 키의 일은 넣은 순서대로 처리된다. 생산자 4개가 각자 자기 키로 2000개씩 동시에 넣는다(Lane 3개라 키가 섞인다).
    // 키마다 기록하는 List 는 그 키의 Lane 만 만지므로 락이 없다. DisposeAsync 뒤에는 락 없이 읽어도 된다.
    [Fact]
    public async Task KeepsOrderPerKey()
    {
        const int ProducerCount = 4;
        const int PerProducer = 2000;
        var seen = new List<int>[ProducerCount];
        var group = new LaneGroup(3);
        var producers = new Task[ProducerCount];
        for (var p = 0; p < ProducerCount; p++)
        {
            var mine = seen[p] = [];
            var lane = group.LaneFor(p);
            producers[p] = Task.Run(() =>
            {
                for (var i = 0; i < PerProducer; i++)
                {
                    var n = i;
                    Assert.True(lane.TryPost(() => mine.Add(n)));
                }
            }, TestContext.Current.CancellationToken);
        }
        await Task.WhenAll(producers);
        await group.DisposeAsync();

        Assert.All(seen, mine => Assert.Equal(Enumerable.Range(0, PerProducer), mine));
    }

    // 그룹을 멈추는 중에 Lane 끼리 일을 넣어도 안전하게 끝난다(C++ 의 Codex 리뷰 R1 과 같은 상황).
    // Lane 1 의 일이 "자기 Lane 에 넣기가 거절될 때"(= DisposeAsync 가 Lane 1 을 닫기 시작)까지 기다린 뒤 Lane 0 에 넣는다.
    // 시간이 아니라 순서로 맞춘다. 그 시점에 Lane 0 은 이미 닫혔으므로 false 여야 한다.
    // C# 은 GC 라 해제된 메모리 문제는 없고, 닫힌 Lane 에 넣으면 false 인지만 본다.
    [Fact]
    public async Task CrossLanePostDuringShutdownIsSafe()
    {
        var result = -1;
        var group = new LaneGroup(2);
        var self = group[1];
        var target = group[0];
        Assert.True(self.TryPost(() =>
        {
            while (self.TryPost(() => { })) // 거절되면 이 Lane 이 닫히기 시작한 것
            {
                Thread.Yield();
            }
            result = target.TryPost(() => { }) ? 1 : 0;
        }));
        await group.DisposeAsync();

        Assert.Equal(0, result);
    }

    [Fact]
    public void RejectsZeroLanes()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new LaneGroup(0));
    }
}
