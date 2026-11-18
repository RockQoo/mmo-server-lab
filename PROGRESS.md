# PROGRESS

서버 코어를 **기술별 정적 라이브러리**로 하나씩 만든다. 라이브러리 하나가 끝날 때마다 **gtest 단위 테스트**로 기능을 검증하고, 확인을 받은 뒤 커밋·push한다. 테스트는 기본 QA(push 전 검사)에 걸려서, 나중에 다른 곳을 고치다 이미 만든 기능이 망가지면 push가 막힌다.

## 설계 결정 (2026-11-18)

- 실행 단위(자기 id와 큐를 가진 데이터 주인)는 **Processor**: `ZoneProcessor`, `DBProcessor`, `LoginProcessor`
- Processor 안에서 같은 데이터를 나눠 처리하는 기능 클래스는 **System**: `MoveSystem`, `CombatSystem`, `AISystem`
- 메시지는 **id로 보내면 항상 같은 스레드**에서 처리된다(순서·정합성 보장, asio strand와 목적이 같음)
- 직렬화는 클래스마다 직접 적되 매크로로 선언한다. 나중에 데이터·패킷 클래스는 CodeGen으로 자동 생성
- **서버는 안전이 먼저다.** 검사(개수 확인, 실패 표시)는 기본으로 넣고, 성능 문제는 벤치마크로 드러나면 그때 고친다
- BinaryWriter/BinaryReader는 바이너리로 만들 수 있는 타입을 전부 지원한다. 쓰기(vector 소유)와 읽기(span, 복사 없음)를 나눈다. 판별은 `if constexpr` 한 곳에서 위에서부터, 지원하지 않는 타입은 `static_assert`로 막는다. 고정 크기 배열도 개수를 쓴다(안전), wstring은 UTF-8로 바꿔 쓴다

상태: ⬜ 예정 · 🔨 진행 중 · ✅ 완료(push됨)

## Core 라이브러리

순서는 의존 관계를 따른다. 위에 있는 라이브러리를 아래에서 쓴다.

| # | 라이브러리 | namespace | 내용 | 검증할 것 | 상태 |
|---|---|---|---|---|---|
| 1 | **Base** | `Base` | `BinaryWriter`/`BinaryReader`(직렬화/역직렬화), 기본 타입, Assert, BuildInfo | 기본 자료형, enum, string, class, STL 컨테이너 왕복 | 🔨 |
| 2 | **Messaging** | `Messaging` | 메시지 큐, 생산자-소비자 N:M, 스레드 어피니티, 내부 통신 | 생산자·소비자 수 조합, 순서 보장, 어피니티 | ⬜ |
| 3 | Thread | `Thread` | 스레드 생성·이름·코어 고정, Lock | lock 종류별 경합 | ⬜ |
| 4 | Memory | `Memory` | ObjectPool, 참조 카운트 | new/delete vs 풀 | ⬜ |
| 5 | Log | `Log` | 비동기 로거 (Messaging 큐 사용) | 동기 vs 비동기 비용 | ⬜ |
| 6 | Metrics | `Metrics` | 카운터, 지연 히스토그램(p50/p99) | 측정 비용 | ⬜ |
| 7 | Timer | `Timer` | 예약 실행, 고정 틱 루프 | 우선순위 큐 vs 타이머 휠 | ⬜ |
| 8 | Network | `Network` | IOCP, Listener, Connector, Session, 송수신 버퍼, 패킷 프레이밍 | I/O 레인 1→N, Nagle, 모아 보내기 | ⬜ |
| 9 | DB | `DB` | ODBC, 커넥션 풀, DB 큐 | 동기 vs 비동기 vs 모아서 쓰기 | ⬜ |
| - | CrashDump | (Base에 포함?) | 크래시 때 미니덤프 저장 | 일부러 크래시 내서 덤프 확인 | ⬜ |
| - | Config | (Base에 포함?) | 설정 파일 읽기 (레인 수, 포트) | 재빌드 없이 벤치마크 조건 변경 | ⬜ |

나중에 비교 실험할 것: RIO(Registered I/O) vs IOCP, mimalloc vs 기본 할당자, DeadlockDetector.

## 그 다음 (Core를 쓰는 쪽)

| 단계 | 내용 | 상태 |
|---|---|---|
| GameServer | 게임 서버. World(이동·전투·AOI·콘텐츠), DB 연동 | ⬜ |
| BotServer | C++ 봇 서버. Core 공유, Behavior Tree 봇, Game과 통신 | ⬜ |
| Viewer | Godot .NET 3D | ⬜ |
| 부하 테스트 | 봇으로 종합 측정 | ⬜ |
| 분산 | 내부 통신을 네트워크로 바꿔 프로세스 분리 | ⬜ |
| C# 서버 | 같은 설계를 C#으로 | ⬜ |
