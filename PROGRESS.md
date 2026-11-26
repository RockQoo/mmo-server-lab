# PROGRESS

서버 코어를 **기술별 라이브러리**로 하나씩 만든다. 같은 주제를 **C++ 먼저, 바로 이어서 C#**으로 만든다. 라이브러리 하나가 끝날 때마다 단위 테스트(gtest / xUnit)로 기능을 검증하고, 확인을 받은 뒤 커밋·push한다. 테스트는 기본 QA(push 전 검사)에 걸려서, 나중에 다른 곳을 고치다 이미 만든 기능이 망가지면 push가 막힌다.

## 설계 결정

### 2026-11-23 개편

- 서버 구조는 **분산 존**(Gateway + Zone, 이후 Coordinator·ServiceServer·DbWriter). 이유와 단계는 `docs/decisions/001-server-architecture.md`
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
| 1 | **Base** | `Base` | `ByteWriter`/`ByteReader`(C#: 코덱 struct), Assert(C#: BCL `Trace.Assert`/`Debug.Assert`), BuildInfo, Utf8(C#: BCL), `StrongId<Tag>`(C++만, C#은 ID마다 record struct), `Ruid`·`RuidGenerator`(전 서버 유일 64비트 ID), `RwLocked<T>`(C++만) | 기본 자료형, enum, string, class, 컨테이너 왕복. **C++ ↔ C# 같은 바이트** | 🔨 | 🔨 |
| 2 | **Messaging** | `Messaging` | 메시지 큐, 생산자-소비자 N:M, 스레드 어피니티(레인), 레인 스레드(이름 ✅, 코어 고정은 벤치마크로) | 생산자·소비자 수 조합, 순서 보장, 어피니티. 객체 큐 vs 직렬화 큐 | 🔨 `Lane`, `LaneGroup` | 🔨 `Lane`, `LaneGroup` |
| 3 | Memory | `Memory` | BufferPool(+ PooledBuffer 핸들), ObjectPool, 참조 카운트(보류: Network에서 `shared_ptr`와 비교) / C#: `ArrayPool`, `Microsoft.Extensions.ObjectPool` | new/delete vs 풀, C# GC 할당량, BufferPool 1판(등급마다 mutex) vs 2판(스레드 캐시 + 전역 묶음) ✅ 2판 채택(journal/bufferpool), 3판은 Network에서. ObjectPool vs new/delete vs C# DefaultObjectPool ✅(journal/objectpool) | 🔨 `BufferPool` 2판, `ObjectPool` | ➖ BCL `ArrayPool`, NuGet `ObjectPool`(Bench만) |
| 4 | Log | `Log` | 비동기 로거 (Messaging 큐 사용) / C#: Serilog (2026-11-26, NuGet 설치는 그때 확인) | 동기 vs 비동기 비용 | ⬜ | ⬜ |
| 5 | Metrics | `Metrics` | 카운터, 지연 히스토그램(p50/p99) | 측정 비용 | ⬜ | ⬜ |
| 6 | Timer | `Timer` | 예약 실행, 고정 틱 루프 | 우선순위 큐 vs 타이머 휠 | ⬜ | ⬜ |
| 7 | Network | `Network` | C++: IOCP / C#: `Socket` + `System.IO.Pipelines`. Listener, Connector, Session, 송수신 버퍼, 패킷 프레이밍 | I/O 레인 1→N, Nagle, 모아 보내기 | ⬜ | ⬜ |
| 8 | DB | `DB` | ODBC / ADO.NET, 커넥션 풀, DB 큐 | 동기 vs 비동기 vs 모아서 쓰기 | ⬜ | ⬜ |
| - | CrashDump | Base (2026-11-26) | 크래시 때 미니덤프 저장 | 일부러 크래시 내서 덤프 확인 | ⬜ | ⬜ |
| - | Config | Base (2026-11-26) | 설정 파일 읽기 (레인 수, 포트). JSON(주석 허용): C++ nlohmann/json(그때 설치), C# `System.Text.Json` (2026-11-26) | 재빌드 없이 벤치마크 조건 변경 | ⬜ | ⬜ |

나중에 비교 실험할 것: RIO(Registered I/O) vs IOCP, mimalloc vs 기본 할당자, DeadlockDetector, UTF-8 검사(단순 구현 vs simdutf vs 끔. 즉석 측정 2026-11-19: 한글 1KB 채팅 읽기의 약 65%가 검사, 이름은 약 15~20%), C++ vs C# 틱 지연 p99·처리량·GC 멈춤, Lane 큐(mutex + 두 버퍼 교체 vs lock-free MPSC vs moodycamel::ConcurrentQueue, C#은 `Channel<T>` 설정별. 생산자 수·일 크기별 처리량과 지연. 처음엔 mutex + 두 버퍼로 만든다, 2026-11-24), Lane 스레드 코어 고정(끔 vs 하드 고정 `SetThreadAffinityMask` vs 힌트 `SetThreadIdealProcessor`, 틱 지연 p99로 비교. 기본은 끔, 2026-11-26), BufferPool 3판(Lane 자체 풀 + 원격 반납함): Network 단위에서 실제 송신 흐름(Lane이 빌리고 IOCP가 반납)으로 2판과 벤치마크해 조금이라도 좋으면 넣는다(2026-11-26 결정, docs/journal/bufferpool.html).

## 나중에 개선·고민할 것 (후순위)

지금 단위를 마무리한 뒤, 관련 단위에서 다시 꺼내 본다. 위 "나중에 비교 실험할 것"과 겹치는 것은 여기에 자세히 적는다. 하나씩 바꾸며 재고(벤치마크), 좋아지면 넣는다.

| 대상 | 무엇 | 왜 | 언제 |
|---|---|---|---|
| BufferPool | **2판 최적화**: ① 빠른 길(내 캐시 꺼내기·넣기)을 헤더 `inline`으로, 느린 길(전역 묶음·새 할당)은 `.cpp`에 ② 스레드 캐시를 소멸자 없는 구조로(MSVC `thread_local` 초기화 검사 제거) ③ 등급 계산을 `std::bit_width`로(반복문 → CPU 명령 한 번). 하나씩 바꾸며 잰다 | C# `ArrayPool`과 스레드 1개 속도가 비슷하다(약 3.5~5ns). C++이면 확실히 빨라야 한다. 함수 호출이 인라인되지 않는 것이 가장 클 것으로 추정. 근거: 같은 구조를 헤더 템플릿(모두 인라인)으로 만든 `ObjectPool`은 스레드 1개 약 1.6ns(625M/s)로 BufferPool(200~290M/s)의 2배 이상이다(2026-11-26) | Memory 다음 단위 전, 또는 Network 전 |
| BufferPool | **C#과 같은 조건으로 비교**: 같은 총 횟수, C# `ArrayPool`에도 "빌리기 ≠ 반납" 추가 | 지금은 C#과 C++ 측정 조건이 달라(C# 8만 회, C++ 840만 회) 여러 스레드 값을 직접 비교하기 어렵다 | 2판 최적화와 같이 |
| BufferPool | **3판**(Lane 자체 풀 + 원격 반납함) | 전역 mutex가 아예 없다. 실제 송신 흐름(Lane이 빌리고 IOCP가 반납)으로 2판과 재서 조금이라도 좋으면 넣는다(2026-11-26 결정) | Network |
| BufferPool | 16KB·64KB 등급 묶음 크기 | 지금 8·2개라 빌리기 ≠ 반납이 많으면 무너질 수 있다. 큰 패킷은 드물어 메모리를 아꼈다 | 큰 패킷이 잦아지면 |
| BufferPool | 반납 버퍼 상한(OS에 돌려주기) | 지금은 풀이 끝까지 들고 있다. 순간 몰림 뒤 메모리를 계속 차지한다 | 실측 메모리가 문제 되면 |
| BufferPool | `PooledBuffer::Release()`(날 포인터 꺼내기) | IOCP에서 핸들 대신 직접 관리가 필요할 수 있다 | Network에서 필요해지면 |
| Memory | **BufferPool·ObjectPool 공통 코드 합치기**(스레드 캐시, 묶음 넣기·꺼내기, 스레드 종료 처리) | 구조가 같아 비슷한 코드가 두 벌이다. 연결 방식이 달라(버퍼 안 덮어쓰기 vs 객체 뒤 연결 칸) 합치면 템플릿이 복잡해질 수 있어 따로 만들었다(2026-11-26) | BufferPool 2판 최적화와 같이 |
| ObjectPool | 타입별 캐시 개수(`ObjectPoolTraits`), 반납 객체 상한 | 지금은 모두 64·32개. 큰 객체·내부 용량이 큰 객체는 메모리를 오래 쥔다 | 실제로 쓰는 타입이 생기면 |
| Memory | `RefCounted`(침투형 참조 카운트) vs `std::shared_ptr` | 세션·방송 버퍼 수명 | Network |
| Messaging | Lane 큐 비교(mutex + 두 버퍼 vs lock-free MPSC 등), Lane 코어 고정 | 위 "나중에 비교 실험할 것" | 벤치마크 때 |

자세한 고민 과정: `docs/journal/bufferpool.html`, `docs/journal/objectpool.html`, 측정 주의: `docs/journal/benchmark.html`.

## 도구

| 항목 | 내용 | 상태 |
|---|---|---|
| CodeGen | `tools/CodeGen`(C# 콘솔). proto → `server-cpp`, `server-cs`, `client-ue5` 코드 생성 | ⬜ |
| 기본 QA | C++·C# 빌드·테스트·lint(`qa.json`), CodeGen 재생성 결과가 커밋본과 같은지 | 🔨 (두 언어 빌드·테스트·lint 완료, CodeGen 비교는 CodeGen 만들 때) |

## 그 다음 (Core를 쓰는 쪽, `docs/decisions/001-server-architecture.md` 단계)

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
