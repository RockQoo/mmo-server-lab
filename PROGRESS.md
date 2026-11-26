# PROGRESS

서버 코어를 **기술별 라이브러리**로 하나씩 만든다. 같은 주제를 **C++ 먼저, 바로 이어서 C#**으로 만든다. 라이브러리 하나가 끝날 때마다 단위 테스트(gtest / xUnit)로 기능을 검증하고, 확인을 받은 뒤 커밋·push한다. 테스트는 기본 QA(push 전 검사)에 걸려서, 나중에 다른 곳을 고치다 이미 만든 기능이 망가지면 push가 막힌다.

## 설계 결정

### 2026-11-23 개편

- 서버 구조는 **분산 존**(Gateway + Zone, 이후 Coordinator·ServiceServer·DbWriter). 이유와 단계는 `docs/decisions/001-서버-구조.md`
- **C++(주력)과 C# 서버를 같이 만든다.** 같은 개념은 같은 클래스 이름과 namespace(`docs/naming.md`). 구현은 각 언어답게: C++은 메시지 큐 + 스레드 어피니티, C#은 async/await + `Channel<T>` + GC 할당 최소화
- 클라이언트는 UE5 5.8 하나, 봇은 C++ BotServer 하나. 같은 프로토콜이라 두 서버에 다 붙는다
- 프로토콜은 `proto/` 하나 → `tools/CodeGen`(C#)이 C++·C# 코드 생성. 생성 코드는 손으로 고치지 않는다
- `RwLocked<T>`(데이터 + 읽기/쓰기 락, `Read()`/`Write()`)는 Base에 둔다. 어피니티로 막지 못하는 자리에서만 쓴다. C#에는 없다
- 스레드 생성·이름·코어 고정은 따로 Thread 라이브러리를 두지 않고 Messaging(레인 스레드)에 넣는다

### 2026-11-18

- 실행 단위(자기 id와 큐를 가진 데이터 주인)는 **Processor**: `ZoneProcessor`, `DBProcessor`, `LoginProcessor`
- Processor 안에서 같은 데이터를 나눠 처리하는 기능 클래스는 **System**: `MoveSystem`, `CombatSystem`, `AISystem`
- 메시지는 **id로 보내면 항상 같은 스레드**에서 처리된다(순서·정합성 보장, asio strand와 목적이 같음). 분산 존에서는 존 id → 존 스레드
- 데이터·패킷 클래스는 CodeGen으로 자동 생성
- **서버는 안전이 먼저다.** 검사(개수 확인, 실패 표시)는 기본으로 넣고, 성능 문제는 벤치마크로 드러나면 그때 고친다
- ASSERT는 Debug·Release 모두 검사한다(실패: 기록 → 디버거면 멈춤 → RaiseException 크래시). 비싼 검사만 DEBUG_ASSERT(Release에서 사라짐)
- ByteWriter/ByteReader는 바이너리로 만들 수 있는 타입을 전부 지원한다. 쓰기(vector 소유)와 읽기(span, 복사 없음)를 나눈다. 판별은 `if constexpr` 한 곳에서 위에서부터, 지원하지 않는 타입은 `static_assert`로 막는다. 고정 크기 배열도 개수를 쓴다(안전), wstring은 UTF-8로 바꿔 쓴다, string을 읽을 때 UTF-8을 검사한다(바이너리는 vector<std::byte>)

상태: ⬜ 예정 · 🔨 진행 중 · ✅ 완료(push됨) · ➖ 해당 없음

## Core 라이브러리

순서는 의존 관계를 따른다. 위에 있는 라이브러리를 아래에서 쓴다.

| # | 라이브러리 | namespace | 내용 | 검증할 것 | C++ | C# |
|---|---|---|---|---|---|---|
| 1 | **Base** | `Base` | `ByteWriter`/`ByteReader`(C#: 코덱 struct), Assert(C#: BCL `Trace.Assert`/`Debug.Assert`), BuildInfo, Utf8(C#: BCL), `RwLocked<T>`(C++만) | 기본 자료형, enum, string, class, 컨테이너 왕복. **C++ ↔ C# 같은 바이트** | 🔨 | 🔨 |
| 2 | **Messaging** | `Messaging` | 메시지 큐, 생산자-소비자 N:M, 스레드 어피니티(레인), 레인 스레드(이름·코어 고정) | 생산자·소비자 수 조합, 순서 보장, 어피니티. 객체 큐 vs 직렬화 큐 | 🔨 `Lane` | 🔨 `Lane` |
| 3 | Memory | `Memory` | ObjectPool, 참조 카운트 / C#: `ArrayPool`, 풀링 | new/delete vs 풀, C# GC 할당량 | ⬜ | ⬜ |
| 4 | Log | `Log` | 비동기 로거 (Messaging 큐 사용) | 동기 vs 비동기 비용 | ⬜ | ⬜ |
| 5 | Metrics | `Metrics` | 카운터, 지연 히스토그램(p50/p99) | 측정 비용 | ⬜ | ⬜ |
| 6 | Timer | `Timer` | 예약 실행, 고정 틱 루프 | 우선순위 큐 vs 타이머 휠 | ⬜ | ⬜ |
| 7 | Network | `Network` | C++: IOCP / C#: `Socket` + `System.IO.Pipelines`. Listener, Connector, Session, 송수신 버퍼, 패킷 프레이밍 | I/O 레인 1→N, Nagle, 모아 보내기 | ⬜ | ⬜ |
| 8 | DB | `DB` | ODBC / ADO.NET, 커넥션 풀, DB 큐 | 동기 vs 비동기 vs 모아서 쓰기 | ⬜ | ⬜ |
| - | CrashDump | (Base에 포함?) | 크래시 때 미니덤프 저장 | 일부러 크래시 내서 덤프 확인 | ⬜ | ⬜ |
| - | Config | (Base에 포함?) | 설정 파일 읽기 (레인 수, 포트) | 재빌드 없이 벤치마크 조건 변경 | ⬜ | ⬜ |

나중에 비교 실험할 것: RIO(Registered I/O) vs IOCP, mimalloc vs 기본 할당자, DeadlockDetector, UTF-8 검사(단순 구현 vs simdutf vs 끔. 즉석 측정 2026-11-19: 한글 1KB 채팅 읽기의 약 65%가 검사, 이름은 약 15~20%), C++ vs C# 틱 지연 p99·처리량·GC 멈춤, Lane 큐(mutex + 두 버퍼 교체 vs lock-free MPSC vs moodycamel::ConcurrentQueue, C#은 `Channel<T>` 설정별. 생산자 수·일 크기별 처리량과 지연. 처음엔 mutex + 두 버퍼로 만든다, 2026-11-24).

## 도구

| 항목 | 내용 | 상태 |
|---|---|---|
| CodeGen | `tools/CodeGen`(C# 콘솔). proto → `server-cpp`, `server-cs`, `client-ue5` 코드 생성 | ⬜ |
| 기본 QA | C++·C# 빌드·테스트·lint(`qa.json`), CodeGen 재생성 결과가 커밋본과 같은지 | 🔨 (두 언어 빌드·테스트·lint 완료, CodeGen 비교는 CodeGen 만들 때) |

## 그 다음 (Core를 쓰는 쪽, `docs/decisions/001-서버-구조.md` 단계)

| 단계 | 내용 | C++ | C# |
|---|---|---|---|
| 1 | Gateway ×1 + ZoneServer ×1 + BotServer (Coordinator는 설정 파일) | ⬜ | ⬜ |
| 2 | ZoneServer ×2, handoff(존 이동) | ⬜ | ⬜ |
| 3 | Coordinator | ⬜ | ⬜ |
| 4 | DbWriter + 데이터별 저장 기준표 | ⬜ | ⬜ |
| 5 | ServiceServer (길드·파티). C++은 직접 만든 방식(Lane), **C#은 Microsoft Orleans(가상 액터)**로 만들어 비교한다(2026-11-26 결정, NuGet 설치는 그때 확인) | ⬜ | ⬜ |
| 6 | 서버 간 전쟁 (재접속 → Gateway 경로 변경) | ⬜ | ⬜ |
| - | UE5 클라이언트 (뷰어) | ⬜ | ➖ |
| - | BotServer (Behavior Tree) | ⬜ | ➖ |
| - | 부하 테스트: 같은 봇으로 두 서버 비교 | ⬜ | ⬜ |
