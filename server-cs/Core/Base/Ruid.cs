using System.Diagnostics;
using System.Globalization;

namespace Base;

// Ruid(RockQoo Unique ID): 모든 서버에서 유일한 64비트 ID. C++ Base::Ruid 와 같은 비트 배치, 같은 값이다.
// 발급은 RuidGenerator 가 한다.
//
// 비트 배치 (위부터)
//   bit 63      Sign       늘 0. SQL Server BIGINT(부호 있음)와 long 에 그대로 들어간다
//   bit 62..22  Timestamp  41비트. 기준 시각(2026-01-01 UTC)부터 지난 ms. 약 69.7년(2095년까지)
//   bit 21..15  Realm      7비트. 플레이어가 고르는 서버 하나("1서버"). 1~127, 0은 예약
//   bit 14..9   Node       6비트. 그 렐름 안에서 ID를 만드는 프로세스 번호. 0~63
//   bit 8..0    Sequence   9비트. 같은 프로세스, 같은 ms 안의 순번. 0~511
//
// C++ 과 다른 점(C#다운 방식):
//   C++ 은 StrongId<Tag> 템플릿을 상속한 class Ruid 다. C# 에서 제네릭에 별칭을 붙이면 진짜 타입이 아니라 프로젝트 안의
//   약칭이라 디버거·에러·로그에 이름이 안 보인다. 그래서 C# 은 ID 마다 readonly record struct 를 둔다
//   (콘텐츠 ID 는 나중에 CodeGen 이 같은 모양으로 만든다). 사용법(Ruid.Init / Ruid.Create / id.Realm)은 C++ 과 같다.
//   record struct 라 같음 비교·해시를 직접 구현해 박싱이 없고, 크기는 long 과 같다.
//
// 주의사항 (C++ Ruid.h 와 같다)
//   1. 비트 배분과 기준 시각은 첫 ID 를 발급한 뒤로 바꾸지 않는다. DB 에 남은 ID 와 뜻이 어긋난다.
//   2. Realm 번호는 모든 지역에서 하나의 번호표로 매긴다. 지역마다 1부터 매기면 서버 통합 때 ID 가 겹친다.
//   3. Node 번호는 같은 렐름에서 동시에 떠 있는 발급 프로세스끼리 달라야 한다. 같으면 같은 ms 에 같은 ID 가 나온다.
//      순번(CAS)은 프로세스 안의 스레드끼리만 나눠 쓰기 때문이다. 꺼진 프로세스의 번호는 다시 써도 된다(시각이 다르다).
//   4. 상한: 렐름 127개, 렐름당 발급 프로세스 64개, 프로세스당 ms 마다 512개(넘으면 다음 ms 까지 기다린다).
//   5. 아래 비트가 Sequence 라서 `id % N` 으로 나누면 한쪽에 몰린다. Lane 등을 고를 때는 비트를 섞는 해시를 쓴다.
//   6. JavaScript(웹 툴)는 2^53 까지만 정확하다. JSON 으로 내보낼 때는 문자열로 쓴다.
//   7. ID 만 보고 만든 시각, 렐름, 노드를 알 수 있다. 클라이언트에 보이는 ID 라면 이 점을 감안한다.
public readonly record struct Ruid : IComparable<Ruid>
{
    public const int TimestampBits = 41;
    public const int RealmBits = 7;
    public const int NodeBits = 6;
    public const int SequenceBits = 9;

    public const int NodeShift = SequenceBits;
    public const int RealmShift = NodeShift + NodeBits;
    public const int TimestampShift = RealmShift + RealmBits;

    public const long MaxTimestamp = (1L << TimestampBits) - 1;
    public const int MinRealm = 1; // 0은 예약(default = 없음, Init 을 빼먹은 프로세스 잡기)
    public const int MaxRealm = (1 << RealmBits) - 1;
    public const int MaxNode = (1 << NodeBits) - 1;
    public const int MaxSequence = (1 << SequenceBits) - 1;

    // 기준 시각 2026-01-01T00:00:00Z 의 유닉스 ms. 바꾸지 않는다(주의사항 1).
    public const long EpochUnixMs = 1_767_225_600_000;

    // 정수에서 만들 때는 이렇게 적어야 한다(new Ruid(5)). 정수로 저절로 바뀌지 않는다.
    // default(Ruid)(0)는 "아직 없음"이다. Realm 0을 예약해서 실제로 발급된 ID 는 0이 될 수 없다.
    public Ruid(long value) => Value = value;

    // DB 바인딩, 로그처럼 정수가 꼭 필요한 곳에서만 꺼낸다.
    public long Value { get; }

    public long Timestamp => (Value >> TimestampShift) & MaxTimestamp;
    public int Realm => (int)((Value >> RealmShift) & MaxRealm);
    public int Node => (int)((Value >> NodeShift) & MaxNode);
    public int Sequence => (int)(Value & MaxSequence);

    // 만든 시각(유닉스 ms)
    public long UnixMs => Timestamp + EpochUnixMs;

    // 프로세스 전역 발급기를 만든다. 서버 시작에 한 번만(두 번이면 Trace.Assert). C++ Ruid::Init 과 같다.
    public static void Init(int realm, int node) => RuidGenerator.InitializeGlobal(realm, node);

    // 새 ID. 어느 스레드에서 불러도 겹치지 않는다. Init 전에 부르면 Trace.Assert. C++ Ruid::Create 와 같다.
    // 가짜 시계·여러 노드가 필요한 테스트는 RuidGenerator 를 직접 만든다.
    public static Ruid Create() => RuidGenerator.Global.Next();

    // 칸 네 개로 ID 를 만든다. 범위를 벗어나면 Trace.Assert 로 끝낸다(잘못된 ID 는 DB 에 영원히 남아 고칠 수 없다).
    // timestamp 는 기준 시각부터 지난 ms 다(유닉스 ms 가 아니다).
    public static Ruid Make(long timestamp, int realm, int node, int sequence)
    {
        Trace.Assert(timestamp is >= 0 and <= MaxTimestamp, "Ruid timestamp out of range");
        Trace.Assert(realm is >= MinRealm and <= MaxRealm, "Ruid realm out of range");
        Trace.Assert(node is >= 0 and <= MaxNode, "Ruid node out of range");
        Trace.Assert(sequence is >= 0 and <= MaxSequence, "Ruid sequence out of range");

        return new Ruid((timestamp << TimestampShift) | ((long)realm << RealmShift) | ((long)node << NodeShift) | (long)sequence);
    }

    // 시각이 맨 위라 값 비교 = 만든 순서 비교다.
    public int CompareTo(Ruid other) => Value.CompareTo(other.Value);

    public static bool operator <(Ruid left, Ruid right) => left.Value < right.Value;
    public static bool operator >(Ruid left, Ruid right) => left.Value > right.Value;
    public static bool operator <=(Ruid left, Ruid right) => left.Value <= right.Value;
    public static bool operator >=(Ruid left, Ruid right) => left.Value >= right.Value;

    // record struct 기본 ToString 은 "Ruid { Value = ... }" 라 정수만 남긴다(C++ std::format 과 같게).
    public override string ToString() => Value.ToString(CultureInfo.InvariantCulture);
}
