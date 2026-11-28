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

## 개발 순서 (2026-11-27 결정)

Core를 전부 만들고 넘어가지 않는다. **1단계(Gateway ×1 + ZoneServer ×1 + BotServer)에 필요한 Core만 만들고 바로 게임 레이어로 간다.** 그 뒤로는 Core와 게임 레이어를 번갈아 키운다(DB는 4단계 DbWriter 때).

| 순위 | 항목 | 라이브러리 | 이유 |
|---|---|---|---|
| 1 | ✅ Lane 개선 1단계(2026-11-27: stop 확인 → `bool`, `condition_variable_any` → `std::condition_variable`, 메모리 배치. 4:4 72.7 → 53.5ns, C# 45.7ns의 약 1.17배. lock-free MPSC는 보류: 아래 후순위) | Messaging | N:M에서 C# `Channel`보다 약 1.5배 느렸다. 위의 모든 것이 Lane 위에서 돈다 |
| 2 | ~~MessageRouter~~ → **2단계(존 이동)로 미룸**(2026-11-27) | Messaging | 설계는 끝남(결정 004). 1단계 ZoneServer 안 Processor는 사실상 `ZoneProcessor` 하나라 보낼 곳이 없다(패킷 입구는 `PacketDispatcher`, 같은 존 안은 직접 호출). 첫 실제 사용처는 존↔존(2단계), 다음이 DbWriter(4)·ServiceServer(5)로 보내기(다른 서버, 직렬화) |
| 3 | Log | Log | 네트워크부터는 로그 없이 디버깅이 어렵다. ✅ C++ 1·2조각(2026-11-27: 카테고리 enum + magic_enum, `.KV()`, 로그 Lane, ConsoleSink, FileSink 날짜별), C# Serilog(Console·File·Async). 한 줄 비용 벤치(performance.html 사례 13: Null 11,980 → 389ns, File 18,330 → 2,228ns, C# 1,122 / 2,639ns). 다음: 리뷰 |
| 4 | CrashDump | Base | 네트워크 개발 중 크래시 덤프. ✅ 1조각(2026-11-27: C++ 덤프 전용 스레드·처리기·수동 Write, C# 수동 Write). 2조각: Full 스냅샷(1GB 멈춤 513 → 21ms). 3조각: 덤프 뒤 로그 flush(afterCrash, 시간 제한). C# `Write`는 일부 복사 실패(`ERROR_PARTIAL_COPY`)면 한 번 다시 쓴다(2026-11-28). 수동 덤프 명령은 관리 콘솔(Network 뒤) |
| 5 | Timer(예약 실행, 고정 틱 루프) | Scheduling | 틱 R&D(노트 계획 G), ZoneServer 바탕. Network와 순서 바꿔도 된다. ✅ 2026-11-28: C++ `TimerHeap`·`Scheduler`(타이머 스레드 + Lane `Post`), C# `Scheduler`(BCL `TimeProvider.CreateTimer` + `Lane.TryPost`), `Base::SetCurrentThreadName`. 힙 vs 휠 벤치 → `TimerHeap` 유지(performance.html 사례 15). namespace는 C# `System.Threading.Timer`와 겹쳐 `Scheduling`. 고정 틱 루프는 ZoneServer 때(`Scheduler::Every`로 충분한지). 다음: 리뷰 |
| 6 | Metrics(카운터, 지연 히스토그램) | Metrics | 부하 테스트의 틱 지연 p99, GC 비교(노트 계획 F). ✅ 2026-11-28: C++ `MetricCounter`(칸 16개)·`LatencyHistogram`(528칸)·`ScopedTimer`·`MetricsReporter`, C# BCL `Meter` 기록 + `MetricsCollector`(`MeterListener`, 칸 16개)·같은 528칸·`ScopedTimer`·`MetricsReporter`. 칸 나누기로 16스레드 C++ 25배, C# 15배(performance.html 사례 16). 측정 비용: 시계 18/16ns, 기록 8.5/7.3ns. 다음: 리뷰 |
| 7 | 직렬화 필드 비용(노트 계획 D) | Base | 패킷마다 쓰인다. ✅ 2026-11-28: ① C++ `WriteBytes`·`ReadBytes` 인라인 + 쓰기 `resize`·`memcpy` ② 통째 복사(C++ `Base::Blittable` 컴파일 검사, C# `Blittable<T, TCodec>` 처음 한 번 비교) → Inventory 쓰기 C++ 789 → 12.9ns, C# 208 → 10.5ns(performance.html 사례 10). 다음: 리뷰 |
| ~~8~~ | ~~CodeGen~~ → **보류**(2026-11-28) | 도구 | 쓸 곳(1단계 게임 메시지)이 아직 없고 패킷 머리·ID 규칙은 Network에서 정해진다. Core를 먼저 만들고 패킷은 손으로 쓰다가, 쌓이면 검토(다시 볼 때: C++·C# 두 벌 맞추기가 반복될 때, UE5 클라 붙을 때, 기획 데이터 표 생길 때). 후보(JSON 통일, 원시 문자열 생성, 파일 나누기, ID 분류+번호, Dispatcher 등록)는 `docs/journal/codegen.html` |
| 8 | Config | Base | 포트·IOCP·Lane 수를 재빌드 없이 바꾸는 틀. 값은 1단계 봇 부하 테스트로 정한다. 처음 읽는 곳이 Network·ZoneServer `main`이라 Network 바로 앞(2026-11-27 순서 변경). nlohmann/json 설치 확인 |
| 9 | Network | Network | 1단계의 핵심. BufferPool 3판, 수신 단위 묶어 넣기(결정 004) |
| 11 | DB | DB | 4단계(DbWriter) 때 |

- **1단계 진입 조건**: 위 1, 3~9번. 끝나면 1단계 게임 레이어: 접속 → 로그인(토큰 없음) → 존 입장 → 이동 브로드캐스트(틱 R&D A vs B 실제 비교) → 공격 1종 → 봇 수백~수천 명 첫 부하 테스트 → UE5 뷰어.
- **틈날 때(막지 않음)**: 풀 N:M 측정(PC가 쉴 때), BufferPool 인라인(노트 계획 C), C# GC 공부(노트 계획 E, Metrics 뒤가 좋다).

## Core 라이브러리

순서는 의존 관계를 따른다. 위에 있는 라이브러리를 아래에서 쓴다.

| # | 라이브러리 | namespace | 내용 | 검증할 것 | C++ | C# |
|---|---|---|---|---|---|---|
| 1 | **Base** | `Base` | `ByteWriter`/`ByteReader`(C#: 코덱 struct), Assert(C#: BCL `Trace.Assert`/`Debug.Assert`), BuildInfo, Utf8(C#: BCL), `StrongId<Tag>`(C++만, C#은 ID마다 record struct), `Ruid`·`RuidGenerator`(전 서버 유일 64비트 ID), `RwLocked<T>`(C++만) | 기본 자료형, enum, string, class, 컨테이너 왕복. **C++ ↔ C# 같은 바이트** | 🔨 | 🔨 |
| 2 | **Messaging** | `Messaging` | 메시지 큐, 생산자-소비자 N:M, 스레드 어피니티(레인), 레인 스레드(이름 ✅, 코어 고정은 벤치마크로) | 생산자·소비자 수 조합, 순서 보장, 어피니티. 객체 큐 vs 직렬화 큐 ✅ 객체(decisions/002) | 🔨 `Lane`, `LaneGroup` | 🔨 `Lane`, `LaneGroup` |
| 3 | Memory | `Memory` | BufferPool(+ PooledBuffer 핸들), ObjectPool, 참조 카운트(보류: Network에서 `shared_ptr`와 비교) / C#: `ArrayPool`, `Microsoft.Extensions.ObjectPool` | new/delete vs 풀, C# GC 할당량, BufferPool 1판(등급마다 mutex) vs 2판(스레드 캐시 + 전역 묶음) ✅ 2판 채택(journal/bufferpool), 3판은 Network에서. ObjectPool vs new/delete vs C# DefaultObjectPool ✅(journal/objectpool) | 🔨 `BufferPool` 2판, `ObjectPool` | ➖ BCL `ArrayPool`, NuGet `ObjectPool`(Bench만) |
| 4 | Log | `Log` | 비동기 로거 (Messaging 큐 사용) / C#: Serilog (2026-11-26, NuGet 설치는 그때 확인) | 동기 vs 비동기 비용 | ✅ `Log`(카테고리, 로그 Lane, Console·File) | ✅ Serilog(+ Async) |
| 5 | Scheduling | `Scheduling` | 예약 실행(`Scheduler`, `TimerHeap`, `TimerId`), 고정 틱 루프(ZoneServer 때) / C#: BCL `TimeProvider.CreateTimer`(TimerHeap 없음) | 우선순위 큐 vs 타이머 휠 ✅ TimerHeap 유지(performance.html 사례 15) | ✅ `TimerHeap`, `Scheduler` | ✅ `Scheduler` |
| 6 | Metrics | `Metrics` | 카운터, 지연 히스토그램(p50/p99), 로그 한 줄(`MetricsReporter`) / C#: BCL `System.Diagnostics.Metrics` + `MetricsCollector` | 측정 비용 ✅, 카운터 칸 나누기 ✅(performance.html 사례 16) | ✅ `MetricCounter`, `LatencyHistogram`, `ScopedTimer`, `MetricsReporter` | ✅ `MetricsCollector`, `LatencyHistogram`, `ScopedTimer`, `MetricsReporter` |
| 7 | Network | `Network` | C++: IOCP / C#: `Socket` + `System.IO.Pipelines`. Listener, Connector, Session, 송수신 버퍼, 패킷 프레이밍 | I/O 레인 1→N, Nagle, 모아 보내기 | ⬜ | ⬜ |
| 8 | DB | `DB` | ODBC / ADO.NET, 커넥션 풀, DB 큐 | 동기 vs 비동기 vs 모아서 쓰기 | ⬜ | ⬜ |
| - | CrashDump | Base (2026-11-26) | 크래시 때 미니덤프 저장 | 일부러 크래시 내서 덤프 확인 | ✅ 덤프 스레드·처리기·수동 Write·스냅샷 | ✅ 수동 Write(재시도 1회), 자동은 런타임 설정 |
| - | Config | Base (2026-11-26) | 설정 파일 읽기 (레인 수, 포트). JSON(주석 허용): C++ nlohmann/json(그때 설치), C# `System.Text.Json` (2026-11-26) | 재빌드 없이 벤치마크 조건 변경 | ⬜ | ⬜ |

나중에 비교 실험할 것: RIO(Registered I/O) vs IOCP, mimalloc vs 기본 할당자, DeadlockDetector, UTF-8 검사(단순 구현 vs simdutf vs 끔. 즉석 측정 2026-11-19: 한글 1KB 채팅 읽기의 약 65%가 검사, 이름은 약 15~20%), C++ vs C# 틱 지연 p99·처리량·GC 멈춤, Lane 큐(mutex + 두 버퍼 교체 vs lock-free MPSC(moodycamel::ConcurrentQueue는 순서 보장을 못 지켜 제외, decisions/003), C#은 `Channel<T>` 설정별. 생산자 수·일 크기별 처리량과 지연. 처음엔 mutex + 두 버퍼로 만든다, 2026-11-24), Lane 스레드 코어 고정(끔 vs 하드 고정 `SetThreadAffinityMask` vs 힌트 `SetThreadIdealProcessor`, 틱 지연 p99로 비교. 기본은 끔, 2026-11-26), BufferPool 3판(Lane 자체 풀 + 원격 반납함): Network 단위에서 실제 송신 흐름(Lane이 빌리고 IOCP가 반납)으로 2판과 벤치마크해 조금이라도 좋으면 넣는다(2026-11-26 결정, docs/journal/bufferpool.html).

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
| Bench | **측정 틀 확장성**: 미리 만든 스레드(`ThreadTeam`)로도 Noop이 물리 8코어 8스레드에서 약 5배까지만 는다. 64B 정렬은 같고, 코어 고정은 오히려 느렸다. BufferPoolBench·ArrayPoolBench도 `ThreadTeam`으로 옮기기 | 확장성 수치를 믿으려면 Noop이 스레드 수만큼 늘어야 한다(지금은 같은 스레드 수의 언어 간 비교만 결론으로 쓴다, journal/objectpool) | BufferPool 2판 최적화와 같이 |
| ObjectPool | 타입별 캐시 개수(`ObjectPoolTraits`), 반납 객체 상한 | 지금은 모두 64·32개. 큰 객체·내부 용량이 큰 객체는 메모리를 오래 쥔다 | 실제로 쓰는 타입이 생기면 |
| Memory | `RefCounted`(침투형 참조 카운트) vs `std::shared_ptr` | 세션·방송 버퍼 수명 | Network |
| Messaging | **Lane 큐 2단계: lock-free MPSC(티켓 링 버퍼 + 세그먼트, 설계는 performance.html 계획 B)**, Lane 코어 고정. 후보는 "Post가 끝난 순서대로" 보장을 지켜야 한다(decisions/003) | 1단계(2026-11-27)로 4:4 72.7 → 53.5ns, 8:4 73.3 → 55.9ns. 같은 날 C# `Lane` 45.7·44.4ns라 약 1.17~1.26배 남았다(1:1·4:1·2:2는 C++이 빠름). 메시지 하나 처리 비용(70~80ns)에 비해 작고 lock-free는 검증·수정이 어려워 보류(사용자 결정). **다시 볼 조건: 봇 부하 테스트에서 Lane 큐가 병목으로 보일 때** | 부하 테스트 뒤 |
| Base | **직렬화 필드 비용**: `WriteBytes`/`ReadBytes` 빠른 길을 헤더 `inline`으로, 쓰기 전 `reserve` | 필드 하나에 4~5ns(Inventory 970B 쓰기 765ns, 읽기 946ns, decisions/002). 패킷 경로에 그대로 쓰인다 | Network 전 |
| Bench | **기존 여러 스레드 벤치를 N:M으로**: ObjectPool·BufferPool CrossThread는 독립된 1:1 쌍 여러 개일 뿐이다. 생산자 N → 소비자 M(키로 섞어 보내기)을 넣는다 | 실무는 여러 스레드가 빌리고 여러 스레드가 반납해 전역 묶음 mutex·할당기 경합이 더 크다(2026-11-27 사용자 지적) | Lane 큐 개선 전 |
| C# | **GC 비용 측정**: 틱 지연 p99·최댓값, `GC.GetTotalPauseDuration()`, 메모리, Workstation vs Server GC | 마이크로 벤치마크는 메시지가 곧 죽어 Gen0만 돈다. C# `new`는 메시지당 32~1080B 할당(decisions/002). 평균 처리량만으로 언어 비교 결론을 내지 않는다 | 봇 부하 테스트 |
| 구조 비교 | **① 존 안 스레드**: 틱 = 존 Lane 메시지 + 틱 끝 브로드캐스트(지금 방향) vs 전용 틱 스레드 + 스냅샷 + 브로드캐스트 스레드. 틱 시작 지연 p99, 틱 시간, 브로드캐스트 시간, CPU, 프로세스당 최대 동접 | 한 주인 스레드로 복사·lock을 없애는 것이 이득이라는 가설(journal/threading). 전용 스레드는 패킷이 몰려도 틱 시각을 지키고 브로드캐스트를 다음 틱과 겹칠 수 있다. 같은 Core·봇·시나리오, 서버 레이어만 다르게(2026-11-27 사용자 제안) | 1단계 끝(Gateway + Zone + 봇) |
| 구조 비교 | **② 서버 구조**: 분산 존(결정 001 A) vs World 캐시형(결정 001 C 홈 서버: World가 DB + 메모리 캐시, Zone은 콘텐츠). 처리량, 응답 지연 p50·p99, 존 이동 시간, DB 부하, Zone이 죽었을 때 잃는 데이터, 동기화 코드 양 | 결정 001을 실측으로 검증. 비교하려다 Core를 고쳐야 하면 Core가 구조를 가정했다는 뜻이라 일반화한다. 이름은 일반 이름만 | 4단계(DbWriter) 뒤 |

자세한 고민 과정: `docs/journal/bufferpool.html`, `docs/journal/objectpool.html`, 측정 주의: `docs/journal/benchmark.html`.

## 도구

| 항목 | 내용 | 상태 |
|---|---|---|
| CodeGen | `tools/CodeGen`(C# 콘솔). proto → `server-cpp`, `server-cs`, `client-ue5` 코드 생성. 보류(2026-11-28): 패킷을 손으로 쓰다가 쌓이면 검토(`docs/journal/codegen.html`) | ⬜ |
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

### 단계별로 들어가는 패턴 (2026-11-27 결정)

같은 존 안의 일은 `ZoneProcessor` Lane 하나에서 끝난다(플레이어 데이터 주인 = Zone). 그래서 락·되돌리기 패턴은 **경계를 넘을 때만**(존↔존, 존↔ServiceServer, 존↔DB, 서버↔서버) 필요하다. 각 단계를 시작할 때 아래를 설계한다.

| 패턴 | 언제 | 이유 |
|---|---|---|
| Actor(Processor + Lane), `MessageRouter`, `PacketDispatcher`, 스케줄러(`Scheduling`), AOI 격자 | 1단계 | 뼈대. AOI 는 게임 레이어 |
| 세션 상태(접속 → 로그인 → 존 안 → 존 이동 중) | 1~2단계 | enum + 전이 함수로 충분. 범용 FSM 틀은 만들지 않는다 |
| 게임 데이터 테이블(기획 데이터, CodeGen 으로 표 클래스) | 아이템·스킬이 생길 때(1단계 끝~2단계) | 1단계는 이동·공격 1종이라 최소한 |
| 요청 제한(세션별 초당 패킷 수) | 1단계 Gateway 에 간단히 | 선택 |
| 요청-응답(Ask) + 타임아웃 | 2단계 | 다른 서버에 묻고 답을 기다린다. 같은 프로세스 Lane 끼리는 필요 없다 |
| Saga(예약 → 확정/되돌리기) | 2단계(존 이동), 5단계(길드), 6단계(전쟁 보상) | 경계를 넘는 일. 결정 003 의 길드 재료 구매 예 |
| 멱등 키(요청 ID, `Ruid`) | 2단계부터 조금씩 | 타임아웃 뒤 재전송, 재접속, 6단계 보상 원장 |
| `UnitOfWork`(요청 하나의 변경을 한 트랜잭션으로) + Outbox(커밋 뒤에만 다른 서버에 알림) | 4단계 | 재화·거래 즉시 트랜잭션(결정 001). 이름은 단수 `UnitOfWork`(패턴 이름, 클래스는 단수) |
| 쓰기 지연 + 변경 표시 | 4단계 | DbWriter 의 데이터별 저장 기준표 |
| 이벤트 버스(Lane 안 Observer) | 퀘스트·업적이 생길 때 | 콘텐츠가 적을 때는 직접 호출로 충분 |
