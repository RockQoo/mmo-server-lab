# 004. 패킷 흐름과 MessageRouter

- 날짜: 2026-11-27 · 종류: 설계 선택(구현할 때 벤치마크를 붙인다)
- 그림: `docs/journal/performance.html#c12`
- 관련: 결정 001(분산 존), 002(같은 프로세스는 객체), 003(Lane 순서 보장)

## 질문

ZoneServer 안에서 패킷은 어떤 길로 Processor(실행 단위, 2026-11-18 이름)까지 가나. 처리 중에 다른 Processor가 필요하면 어디서 판단해 어떻게 보내나.

## 결정

### 패킷 흐름

```
소켓 → IOCP 스레드 N개 → (자르기·검사·역직렬화) → PacketDispatcher → LaneGroup의 Lane M개(Processor) → SendQueue → WSASend
```

1. **역직렬화와 검사는 IOCP 스레드에서 한다.** 패킷 자르기(`PacketFramer`), 크기·형식 검사, 역직렬화를 IOCP 스레드 N개가 나눠 하고 Lane에는 완성된 객체만 넘긴다(결정 002). Lane은 게임 로직만 하고, 잘못된 패킷은 Lane까지 가지 않는다.
2. **수신 하나에 든 패킷을 Lane별로 모아 한 번에 넣는다.** Gateway → Zone 연결은 소켓 몇 개에 많은 플레이어의 패킷이 섞여 오므로 효과가 크다(Lane 잠금 횟수가 줄어든다, 결정 002의 병목). **다음 수신을 걸기 전에 반드시 내보낸다.** 그래야 결정 003의 순서 보장이 지켜진다.
3. **목적지 판단은 두 곳이다.**
   - 입구: IOCP 스레드의 `PacketDispatcher`가 "패킷 번호 → (Processor 종류, 키를 어디서 꺼내나)" 표로 고른다. 표는 CodeGen이 proto에서 만든다(예: `MoveReq` → Zone, 키 = 세션의 zoneId). 세션의 경로(playerId, zoneId)는 로그인·존 이동 때 정해지고, IOCP 스레드는 읽기만 한다. 존 이동 중 도착한 패킷은 모아 둔다(결정 001 2단계).
   - 안쪽: Processor의 콘텐츠 코드가 `MessageRouter`로 보낸다(아래).
4. **스레드 수(IOCP N, Lane M)는 Config로 두고 봇 부하 테스트로 정한다.** N + M이 코어 수를 넘지 않게 한다.

### MessageRouter (사용법 ① A + B 합치기)

```cpp
// 쓰는 쪽: 목적지가 코드에 보인다(A)
router.Post<GuildProcessor>(guildId, BuyMaterialReq{ .itemId = 7, .replyTo = playerId });

class GuildProcessor
{
public:
    static constexpr ProcessorKind Kind = ProcessorKind::Guild; // B: 안쪽 표와 다른 서버로 보낼 때
    using Key = GuildId;
    void On(const BuyMaterialReq& req);
};
```

- 컴파일 검사: 키 타입이 `P::Key`가 아니면, `P`에 `On(메시지)`가 없으면 컴파일 에러. 실행 비용 0.
- 실행: `Kind`로 배열에서 `LaneGroup`을 찾고(배열 한 칸) → `LaneFor(키)` → `Lane::Post`. 다른 서버에 있으면 직렬화해서 보낸다(결정 002).
- C#: `router.Post<GuildProcessor, BuyMaterialReq>(guildId, msg)`, 제네릭 제약 `IProcessor<TKey>`, `IHandle<TMsg>`로 같은 검사.
- 큰 메시지(MSVC `move_only_function` 작은 버퍼 56B 초과)는 ObjectPool 핸들로 넘겨 할당을 피한다.
- **보충(2026-11-27)**: 예시의 `GuildProcessor`는 ServiceServer(5단계, 결정 001)에 있다. ZoneServer에서 `Post<GuildProcessor>`는 같은 프로세스의 Lane이 아니라 **다른 서버로 보내기**(`Kind`로 서버를 찾아 직렬화)다. 1단계 ZoneServer 안의 Processor는 사실상 `ZoneProcessor` 하나라서 MessageRouter는 **2단계(존 이동, 존↔존)** 때 만든다. 그때 같은 프로세스 예시는 `router.Post<ZoneProcessor>(targetZoneId, HandoffReq{...})`가 된다.
- 이름 후보 비교: `MessageRouter`(채택, 가장 흔하고 뜻이 바로 보임), `ProcessorRegistry`(등록·조회에 치우침), `ActorSystem`(우리 `System` 기능 클래스 이름과 겹침).
- 사용법 비교: A 템플릿만(키·메시지 실수를 못 막음), B enum만(실행 중에만 검사), C 키 타입이 목적지(간결하지만 목적지가 코드에 안 보임). 사용자는 목적지가 보이는 A와 다른 서버에도 쓰는 B를 원했고, 둘의 단점을 컴파일 검사로 메운 ①을 골랐다.

## R&D로 남긴 것: 틱을 어디서 돌리나

| | A. 틱을 Lane의 일로 | B. 별도 틱 스레드 + 입력 쌓기 + 브로드캐스트 스레드 |
|---|---|---|
| 데이터 주인 | Lane | 틱 스레드(Lane은 입력만 쌓고 월드를 직접 고치지 않는다) |
| 공정성 | 패킷이 오는 대로 | 틱 단위로 똑같이(이동은 마지막 것만, 스킬은 틱당 한 번 등 정리 가능) |
| 틱 간격 | 메시지 뒤로 밀릴 수 있다 | 일정하게 두기 쉽다 |
| 입력 지연 | 바로 | 최대 틱 하나 |

사용자는 B를 생각하고 있다(로직 처리 → 틱이 구간별로 처리 → 브로드캐스트가 일괄 통보). R&D에서 정할 것:

- 틱 입력으로 갈 패킷(이동, 스킬)과 Processor Lane에서 바로 처리할 패킷(인벤토리, 거래, 채팅)을 나누는 기준.
- 둘이 같은 데이터(예: 전투 중 아이템 사용으로 HP 회복)를 건드릴 때 누가 주인인가.
- 측정: 같은 봇 부하로 A와 B의 틱 간격 흔들림(p99), 입력 지연, 처리량, 브로드캐스트 비용.

## Trade-off

- 얻은 것: Lane은 게임 로직만 해서 가볍다. 묶어 넣기로 Lane 잠금이 준다. 콘텐츠 코드는 목적지가 같은 프로세스인지 다른 서버인지 몰라도 되고, 잘못 보내는 실수를 컴파일러가 막는다.
- 잃은 것: IOCP 스레드가 세션의 경로를 읽어야 해서, 존 이동 때 경로를 바꾸는 순서를 신경 써야 한다. 콘텐츠 코드가 받는 Processor의 선언(`Kind`, `Key`, `On`)을 알아야 한다(필요하면 가벼운 헤더로 나눈다).
- 결론이 깨지는 조건: 한 존이 너무 무거워 Lane 하나로 감당이 안 되면 존을 더 잘게 나누거나 틱 방식(B)의 병렬화를 따로 설계한다.
