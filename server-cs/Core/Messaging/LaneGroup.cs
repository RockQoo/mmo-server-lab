namespace Messaging;

// Lane 여러 개를 묶고, 키로 Lane 하나를 고른다. 같은 키는 항상 같은 Lane 으로 가므로
// "같은 키의 일은 넣은 순서대로 하나씩"이 지켜진다(다른 키끼리는 다른 Lane 에서 동시에 돈다). C++ Messaging::LaneGroup 과 같은 역할이다.
//
// 고르는 방법 두 가지
//   LaneFor(key)  많고 모양을 모르는 키(Ruid.Value 등). 키를 섞은(splitmix64) 뒤 Lane 수로 나눈다.
//   this[index]   적고 오래 사는 대상(존). 만들 때 번호를 정해 Lane 을 받아 두고 계속 쓴다.
//
// 섞는 이유: Ruid 처럼 아래 비트가 순번인 ID 는 드물게 만들면 순번이 늘 0이라 key % 8 이 늘 0이다(전부 Lane 0).
// 섞기는 C++ 과 같은 공식이라 두 서버가 같은 키에 같은 번호의 Lane 을 고른다(IndexFor 테스트가 같은 값을 확인).
// HashCode 는 실행마다 값이 달라서 쓰지 않는다.
//
// C++ 과 다른 점(C#다운 방식):
//   이름이 없다. C# Lane 은 전용 스레드 없이 스레드 풀에서 돌아 스레드 이름을 붙일 곳이 없다.
//   번호로 고르기는 인덱서(group[3]), 키는 long 하나로 받는다(Ruid 는 id.Value 를 넘긴다. Messaging 이 Base 를 몰라도 된다).
//   잘못된 개수·번호는 예외(C# 관례. C++ 은 ASSERT).
//
// 순서는 같은 키 안에서만 지켜진다. 키 A 의 일과 키 B 의 일 사이의 순서는 보장하지 않는다.
public sealed class LaneGroup : IAsyncDisposable
{
    private readonly Lane[] _lanes;

    // Lane 을 count 개 만든다. 개수는 벤치마크로 정하고, 나중에 Config 에서 받는다.
    public LaneGroup(int count)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(count);
        _lanes = new Lane[count];
        for (var i = 0; i < count; i++)
        {
            _lanes[i] = new Lane();
        }
    }

    public int Count => _lanes.Length;

    // 번호로 Lane 을 고른다(0 ~ Count-1). 범위를 벗어나면 IndexOutOfRangeException.
    public Lane this[int index] => _lanes[index];

    // 키로 Lane 을 고른다. 같은 키는 항상 같은 Lane.
    public Lane LaneFor(long key) => _lanes[IndexFor(key, _lanes.Length)];

    // 키가 몇 번 Lane 으로 가는지(LaneFor 가 쓰는 계산). 분산 테스트, C++ 과 같은 결과인지 확인에 쓴다.
    public static int IndexFor(long key, int count)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(count);
        return (int)(Mix(unchecked((ulong)key)) % (ulong)count);
    }

    // 모든 Lane 을 멈춘다. 각 Lane 은 남은 일을 처리한 뒤 끝난다.
    public async ValueTask DisposeAsync()
    {
        foreach (var lane in _lanes)
        {
            await lane.DisposeAsync().ConfigureAwait(false);
        }
    }

    // splitmix64 의 마무리 단계. 공식 그대로 옮겼다(바꾸면 C++ 과 결과가 달라진다).
    private static ulong Mix(ulong x)
    {
        unchecked
        {
            x ^= x >> 30;
            x *= 0xBF58_476D_1CE4_E5B9;
            x ^= x >> 27;
            x *= 0x94D0_49BB_1331_11EB;
            x ^= x >> 31;
            return x;
        }
    }
}
