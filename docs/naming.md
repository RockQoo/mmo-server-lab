# C++ ↔ C# 이름 대응표

같은 개념이면 클래스 이름과 namespace를 같게 한다. 한쪽에만 있는 것은 이유를 적는다. 구현이 끝난 것만 "있음"으로 적는다.

앞으로 만들 것까지 포함한 전체 계획 표는 `docs/class-map.html`(제안 이름 포함, 단위마다 갱신). 이 파일은 확정된 이름만 적는다.

## 공통 규칙

| 항목 | C++ (`server-cpp/`) | C# (`server-cs/`) |
|---|---|---|
| namespace | 폴더 이름 = namespace: `Base`, `Messaging` | **C++과 같다**: `namespace Base;` |
| 클래스, 메서드 | Pascal | Pascal |
| 멤버 변수 | `_camel` | `_camel` |
| static / 스레드별 | `s_camel` / `t_camel` | 같음 |
| 파일 | `ByteWriter.h` + `ByteWriter.cpp` | `ByteWriter.cs` |
| 라이브러리 단위 | 정적 라이브러리 `Base` | 프로젝트 `Base.csproj` |
| 테스트 | `Core/Base/Tests/`, gtest, `BaseTests` | `Core/Base/Tests/`, xUnit, `Base.Tests` |

## 클래스

| namespace | 이름 | C++ | C# | 메모 |
|---|---|---|---|---|
| Base | `ByteWriter` / `ByteReader` | 있음(`<<`, `>>`, 버퍼 소유) | 있음(정수·enum·bool·실수·string, 코덱으로 배열·List·nullable·Dictionary·클래스) | C#은 `IBufferWriter<byte>`에 쓰는 ref struct 커서(버퍼는 .NET이 소유: `ArrayBufferWriter`, 나중에 `PipeWriter`). 같은 바이트는 `testdata/bytes/*.hex`로 양쪽 테스트가 확인. .NET `System.IO.BinaryWriter`와 겹쳐 C++ 이름을 Binary→Byte로 바꿈 |
| Base | `Blittable` | 있음(concept `Base::Blittable<T>`: bool 뺀 기본형, 또는 필드가 모두 blittable·선언 순서·크기 합 = sizeof 인 `SERIALIZE_FIELDS` 클래스. 컴파일할 때 검사) | 있음(`Blittable<T, TCodec>.IsBlittable`: 코덱이 `IsPlainCopy => true`로 밝혔을 때만, 타입마다 처음 한 번 메모리를 무늬로 채워 코덱으로 쓴 것·읽은 것과 비교, `static readonly`) | 같은 역할: 메모리를 그대로 복사해도 필드마다 쓴 것과 같은 바이트인 타입. 맞으면 배열(C++ `vector`·`array`·`T[N]`, C# 배열·`List`)을 `memcpy` 한 번으로. 쓰는 코드·바이트 형식은 그대로. 이름은 .NET 용어 blittable(메모리 그대로 복사해도 되는 타입). C# 은 코덱 필드 순서를 컴파일할 때 알 수 없어 방법이 다르다(performance.html 사례 10) |
| Base | `BuildInfo` | 있음(함수 `BuildInfo()`) | 있음(`BuildInfo.Describe()`) | C#은 자유 함수가 없어 정적 클래스. C++은 컴파일러·`__cplusplus`, C#은 런타임·GC 모드(Server/Workstation)를 남긴다 |
| Base | `ASSERT` / `DEBUG_ASSERT` | 있음(매크로, 직접 만듦) | BCL `Trace.Assert` / `Debug.Assert`(만들지 않음) | 같은 역할: 항상 검사 / Debug만 검사, 실패하면 기록 → 디버거면 멈춤 → 잡을 수 없는 종료. C++은 RaiseException(0xE000A55E), C#은 FailFast(0x80131623). 덤프는 C# 은 런타임 설정(`DOTNET_DbgEnableMiniDump=1`), C++ 은 나중에 CrashDump |
| Messaging | `Lane` | 있음(mutex + 두 버퍼 교체 큐, 전용 `std::jthread`, `Post`→bool, `Stop`) | 있음(BCL `Channel<Action>` + 스레드 풀 async 루프, `TryPost`→bool, `DisposeAsync`) | 같은 역할: 넣은 순서대로 한 번에 하나씩, 멈출 때 남은 일 처리, 멈춘 뒤 넣기 거부, 일이 던지면 프로세스 종료(C++ terminate / C# FailFast). C#은 스레드가 고정되지 않는다(쉬는 동안 스레드를 잡지 않음). bool 반환이라 C#은 관례대로 Try 접두사 |
| Messaging | `LaneGroup` | 있음(`LaneGroup(name, count)`, `LaneFor(key)`·StrongId, `At(i)`, 스레드 이름 `"name-i"`, 소멸자는 모두 멈춘 뒤 해제) | 있음(`new LaneGroup(count)`, `LaneFor(long)`, 인덱서 `[i]`, `DisposeAsync`) | 같은 역할: 같은 키 → 같은 Lane. 키는 splitmix64 마무리로 섞은 뒤 Lane 수로 나눔(두 언어가 같은 번호). C#은 스레드 풀이라 이름이 없다 |
| Base | `Ruid` | 있음(`class Ruid : public StrongId<RuidTag>`, `Ruid::Init`/`Ruid::Create`, 칸은 멤버 `id.Realm()`) | 있음(`readonly record struct Ruid`, `Ruid.Init`/`Ruid.Create`, 칸은 멤버 `id.Realm`, `RuidCodec`) | 모든 서버에서 유일한 64비트 ID: Sign 0, Timestamp 41(2026-01-01 기준 ms), Realm 7, Node 6, Sequence 9. 같은 칸이면 두 언어가 같은 정수. 0 = 없음(Realm 0 예약) |
| Base | `RuidGenerator` | 있음(`std::atomic` CAS, `Clock` 함수 주입, `alignas`) | 있음(`Interlocked.CompareExchange`, BCL `TimeProvider`, 패딩 구조체) | 같은 역할: (시각, 순번)을 원자 변수 하나에 담아 CAS, 단조 시계, 512개 소진 시 다음 ms까지 양보, `InitializeGlobal`/`Global` |
| Log | 로그 함수·`Logger` | 있음(namespace `Log`: `Log::Initialize(options, sinks)`, `Log::Info(카테고리 enum, "문장").KV(키, 값)`, `Shutdown`/`Flush`. `LogSink`·`ConsoleSink`·`FileSink`, 로그 전용 Lane) | 있음(`Log.Logger`: `Initialize(LogOptions, sinks)`, `For(카테고리 enum)` → Serilog `ILogger`, `Shutdown`) | 같은 역할: 레벨 6개(VRB~FTL), 카테고리 `[Battle]`, 키·값, 날짜별 파일(`zone-.log` → `zone-20261127.log`), 같은 줄 모양. C#은 Serilog(메시지 템플릿 `attacker={Attacker}`), 파일:줄 없음. C# 은 namespace `Log` 와 같은 이름의 클래스를 둘 수 없어(CA1724) `Logger`. `Serilog.Core` 를 쓰는 파일(sink 구현)에서는 `Serilog.Core.Logger` 와 겹쳐 `using Logger = Log.Logger;` 로 고른다 |
| Base | `CrashDump`, `DumpKind` | 있음(`CrashDump::Install(options)`: 덤프 전용 스레드 + 처리기, `CrashDump::Write(Mini/Full, 이유)`) | 있음(`CrashDump.Write(kind, reason, dir)`만) | 같은 역할: 파일 이름 `<프로세스>_<날짜>_<시각>_pid<번호>_<이유>.dmp`. C# 자동 덤프는 코드가 아니라 런타임 환경 변수(`DOTNET_DbgEnableMiniDump` 등, 시작 때만 읽음)라 `Install` 이 없다. 수동은 MS 공식 `DiagnosticsClient.WriteDump` |
| Base | `ConfigReader`, `CONFIG_FIELDS` | 있음(`Read<T>(path)` / `Parse<T>(text, sourceName)` → `std::expected<T, std::string>`, nlohmann/json. 읽은 구조체를 다시 JSON 으로 만들어 원본과 비교해 알 수 없는 키·담을 수 없는 값을 찾는다) | 있음(정적 클래스 `Read<T>` / `Parse<T>`, BCL `System.Text.Json` 옵션만: `UnmappedMemberHandling.Disallow`, 모든 속성 필수(타입 정보 수정자), `RespectNullableAnnotations`. 실패는 `InvalidDataException`) | 같은 역할: 같은 JSON 파일(키 camelCase)을 같은 규칙으로 읽는다. 주석 허용, 끝 쉼표 금지, 빠진 키·알 수 없는 키·범위 밖 값·타입 다름은 실패. C# 은 시작 때 한 번이라 예외가 자연스럽고, `CONFIG_FIELDS` 는 C# 에 필요 없다(속성이 곧 키) |
| Base | `SetCurrentThreadName` | 있음(함수 `Base::SetCurrentThreadName(name)`, UTF-8 → `SetThreadDescription`, OS 코드는 `ThreadName.cpp` 한 곳. Lane·Scheduler·CrashDump 가 쓴다) | **없음** | 아래 참고 |
| Base | `StrongId<Tag>` | 있음(템플릿, `std::hash`·`std::formatter`·ByteWriter 지원) | **없음**(ID마다 `record struct`) | 아래 참고 |
| Base | `RwLocked<T>` | 예정 | **없음** | 아래 참고 |
| Memory | `BufferPool` / `PooledBuffer` | 있음(2판: 전역 하나 `Shared()`, 스레드 캐시 + 전역 묶음 이동, 크기 등급 5개 256B~64KB, RAII 반납 핸들) | **없음**(BCL `ArrayPool<byte>.Shared`) | 아래 참고 |
| Memory | `ObjectPool<T>` / `PooledPtr<T>` | 있음(템플릿, 타입마다 전역 하나 `Shared()`, 스레드 캐시 64 + 전역 묶음 32, 반납 때 `Reset()`, 핸들은 `unique_ptr` 별칭 8B) | **없음**(MS NuGet `Microsoft.Extensions.ObjectPool`의 `DefaultObjectPool<T>` + 정책 `Return`) | 같은 역할: 객체를 소멸시키지 않고 비워서 다시 쓴다. 아래 참고 |
| Scheduling | `Scheduler` | 있음(타이머 스레드 하나(`std::jthread`) + `TimerHeap` + 시각이 되면 담당 Lane 에 `Post`. `After`/`Every`/`Cancel`/`Stop`) | 있음(BCL `TimeProvider.CreateTimer`(런타임 타이머) + 콜백은 `Lane.TryPost` 만. `After`/`Every`/`Cancel`/`Dispose`, 테스트는 `FakeTimeProvider`) | 같은 역할: "몇 초 뒤", "몇 초마다" 일을 등록할 때 넘긴 Lane 에서 실행, 어느 스레드에서 불러도 됨, `Cancel` 이 true 면 Lane 에 이미 넣은 회차까지 막음. 주기 일이 크게 늦을 때 규칙만 다르다(C++ 다음 칸으로 건너뜀, C# 런타임 규칙: 1ms 뒤 한 번 실행 후 거기서 다시 셈. `docs/journal/performance.html` 사례 15). namespace 를 `Timer` 로 하지 않은 것은 C# `System.Threading.Timer` 와 겹쳐서다 |
| Scheduling | `TimerHeap` | 있음(`priority_queue` + `unordered_map`, 같은 시각은 넣은 순서, 주기는 고정 간격, `Cancel` 은 표시만, 스레드 안전하지 않음) | **없음** | 아래 참고 |
| Scheduling | `TimerId` | 있음(`using TimerId = Base::StrongId<struct TimerTag, uint64_t>`, 0 = 없음) | 있음(`readonly record struct TimerId(ulong Value)`, `default` = 없음) | `Scheduler`(C++ 은 `TimerHeap` 도)가 발급하는 번호. 1부터 |
| Metrics | `MetricCounter` | 있음(늘기만 하는 횟수. 캐시 줄마다 띄운 칸 16개, 스레드마다 정한 칸(`thread_local`)에 `fetch_add`, `Value()`는 칸 합) | **없음**(BCL `System.Diagnostics.Metrics.Counter<long>` 에 기록, 합은 `MetricsCollector` 가 CPU 번호로 고른 칸 16개에 모음) | 같은 역할: 여러 스레드가 같이 올리는 횟수. C++ 이름은 BCL `Counter<T>` 와 겹치지 않게 `MetricCounter`. 칸 나누기 효과는 `docs/journal/performance.html` 사례 16 |
| Metrics | `LatencyHistogram`, `HistogramSnapshot` | 있음(16 미만은 정확, 그 위는 2의 거듭제곱 구간마다 16칸, 528칸, 오차 6.25% 이하. `Record`, `Snapshot`, `SnapshotAndReset`) | 있음(같은 칸 나누기, `Interlocked`. `HistogramSnapshot` 은 `readonly record struct`. 기록은 BCL `Histogram<long>`(ns) → `MetricsCollector` 가 이 칸에 넣음) | 같은 값 → 같은 칸 번호(두 언어 테스트로 확인, 100만 ns → 270번)라 두 언어의 p99 를 그대로 비교한다. BCL `Histogram<T>` 은 분위수를 계산하지 않아 C#도 직접 만듦 |
| Metrics | `ScopedTimer` | 있음(`steady_clock`, 소멸자에서 `LatencyHistogram::Record`) | 있음(`readonly struct`, `IDisposable`, `Stopwatch.GetTimestamp` 차이를 ns 로, `Histogram<long>.Record`) | 블록 하나의 시간을 잰다(스코프/`using`). C#은 `TimeSpan`(100ns 단위)을 거치지 않는다 |
| Metrics | `MetricsReporter` | 있음(`Add(name, MetricCounter&)`, `Add(name, LatencyHistogram&)`, `Report(now)`) | 있음(`AddCounter(name)`, `AddHistogram(name)`, `Report()`, 시계는 `TimeProvider`) | 같은 역할, 같은 줄: `tick.latency=[n=600 p50=812.0us p99=3.10ms max=12.00ms] packets.received=1200/s`. 글만 만들고 로거는 모른다(서버가 `Scheduler.Every(10s)` 로 불러 자기 카테고리로 남긴다). C#은 이름으로 모으므로 종류별 `Add` 로 나눴다 |

## 한쪽에만 있는 것

### `StrongId<Tag>` (C++만)

종류가 다른 ID끼리 섞이지 않게 하는 정수 ID 템플릿(`using PlayerId = StrongId<struct PlayerTag>`). 생성자는 explicit, 같은 Tag끼리만 비교한다. 비용은 정수와 같다.

C#에 만들지 않는 이유: C#의 `using` 별칭은 진짜 타입이 아니라 프로젝트 안의 약칭이라 디버거·에러·로그에 `StrongId<RuidTag>`로 보이고, ID 전용 멤버(`id.Realm`)를 둘 수 없다. 그래서 C#은 ID마다 `readonly record struct`(같음 비교·해시가 박싱 없이 자동)를 두고, 콘텐츠 ID는 CodeGen이 같은 모양으로 만든다(2026-11-26 결정).

### `RwLocked<T>` (C++만)

데이터와 읽기/쓰기 락을 묶어, 락 없이는 값에 접근할 수 없게 하는 래퍼(`Read()`, `Write()`). 스레드 어피니티로 막지 못하는 자리에서만 쓴다.

C#에 만들지 않는 이유:

- C# 서버는 공유 상태를 존마다 `Channel<T>` 하나와 소비자 하나로 처리해 락이 필요 없게 설계한다.
- `lock`, `ReaderWriterLockSlim`은 스레드에 묶여 있어 `await`를 사이에 두고 쥘 수 없다(`await` 뒤 다른 스레드에서 이어질 수 있다).
- 재진입·승급 검사는 `ReaderWriterLockSlim(LockRecursionPolicy.SupportsRecursion)`이 이미 제공한다. 꼭 필요한 동기 락은 `System.Threading.Lock`, 비동기 구간은 `SemaphoreSlim`.

### `BufferPool` / `PooledBuffer` (C++만)

송수신·직렬화 버퍼를 다시 쓰는 풀과, 빌린 버퍼를 소멸할 때 자동으로 돌려주는 핸들(이동만, 복사 금지). 핸들은 반납 빠뜨림, 두 번 반납, 크기 틀린 반납을 막는다.

C#에 만들지 않는 이유(2026-11-26 결정):

- 풀은 BCL `ArrayPool<byte>.Shared`(스레드별 캐시 포함)가 이미 있다. 쓰는 법은 `Rent` + `try/finally Return`.
- 핸들이 실수를 막는 힘은 소멸자와 "이동만"에서 나오는데 C#에는 둘 다 없다. `struct`로 감싸면 대입이 복사라 두 번 반납이 그대로 남고, `class`로 감싸면 빌릴 때마다 할당이 생긴다.
- 실수의 결과도 가볍다(C++은 정의되지 않은 동작, C#은 GC 덕에 메모리는 안전). 네트워크 버퍼는 나중에 `System.IO.Pipelines`가 풀링한다.

### `ObjectPool<T>` / `PooledPtr<T>` (C++만)

객체를 소멸시키지 않고 반납할 때 `Reset()`(noexcept, concept `Poolable`로 강제)만 해서 다시 쓰는 풀. 내부 `vector` 등의 용량까지 재사용한다. 핸들 `PooledPtr<T>`는 `std::unique_ptr<T, ObjectPool<T>::Deleter>` 별칭(상태 없는 deleter라 8B). 구조는 BufferPool 2판과 같다(스레드 캐시 64개 + 전역 묶음 32개, 타입마다 `ObjectPoolTraits<T>`로 바꿀 수 있다).

C#에 만들지 않는 이유(2026-11-26):

- MS 공식 `Microsoft.Extensions.ObjectPool`(`DefaultObjectPool<T>` + `PooledObjectPolicy<T>.Return`이 C++ `Reset()` 자리)이 같은 역할이다. 쓰는 법은 `Get` + `try/finally Return`.
- 다만 측정해 보니 `DefaultObjectPool`은 스레드 캐시가 없어(공유 배열 + Interlocked) 스레드가 늘수록 느려지고, 작은 객체는 그냥 `new`(GC 할당)가 더 빠르다(스레드 1개: 풀 약 10ns, `new` 약 1.4~2.1ns, 8개에서는 풀이 약 130배 느림). 그래서 C#은 **만들기 비싼 객체(큰 버퍼, StringBuilder 등)만 풀에 넣고, 작은 객체는 `new`** 한다. 측정은 `docs/journal/objectpool.html`.

### `TimerHeap` (C++만)

시각 순서로 정리된 약속 목록(`Add(due, job, period)`, `Cancel(id)`, `RunDue(now)`, `NextDue()`). 시계도 스레드도 없다: 부르는 쪽이 "지금"을 넘긴다. 존 틱이 틱 시각으로 직접 부르고(버프 만료, 리스폰), `Scheduler` 의 타이머 스레드가 안에 하나 둔다. 같은 시각은 넣은 순서대로라 결과가 늘 같다. 타이머 휠과 비교해 이 구조를 둔 이유는 `docs/journal/performance.html` 사례 15(2026-11-28).

C#에 만들지 않는 이유(2026-11-28):

- C# `Scheduler` 는 BCL `TimeProvider.CreateTimer`(안은 런타임 `TimerQueue`)가 시각 순서 관리와 깨우기를 맡는다. 같은 일을 하는 목록을 다시 만들 이유가 없다.
- C++ 은 표준에 타이머가 없어(`std::chrono` 는 시계뿐) 직접 만든다.
- 존 틱 안 타이머(C++ 은 존이 `TimerHeap` 을 직접 가짐)의 C# 자리는 ZoneServer 단위 때 정한다(후보: BCL `PriorityQueue<TElement, TPriority>`, 아직 정하지 않음).

### `SetCurrentThreadName` (C++만)

지금 스레드에 이름을 붙이는 함수(UTF-8). 표준에 없어 OS 함수(`SetThreadDescription`)를 쓰고, 그 코드는 `Base/ThreadName.cpp` 한 곳에 둔다. 이름은 크래시 덤프(WinDbg `~`), VS 스레드 창, 프로파일러에 보인다(2026-11-28: Mini 덤프에 이름이 남는 것을 확인). `Lane`(`"Zone-3"`), `Scheduler`, `CrashDump` 의 전용 스레드가 쓴다.

C#에 만들지 않는 이유: C# `Lane` 과 `Scheduler` 는 전용 스레드 없이 스레드 풀에서 돈다(`LaneGroup` 처럼 이름이 없다). 풀 스레드는 일마다 바뀌어 이름을 붙여도 뜻이 없다. 전용 스레드가 필요하면 BCL `Thread.Name` 이 있다.

## C# 에만 있는 것

### `MetricsCollector` (C#만)

BCL `System.Diagnostics.Metrics`(`Meter`, `Counter<long>`, `Histogram<long>`)로 기록한 값을 `MeterListener` 로 받아 서버 안에서 모은다(`CounterValue(name)`, `Snapshot(name)`, `SnapshotAndReset(name)`). BCL 로 기록하는 이유는 MS 도구(`dotnet-counters`)로 밖에서 바로 볼 수 있어서다. 다만 BCL 은 값을 받기만 하고 합이나 p99 를 내 주지 않는다. 콜백은 기록한 스레드에서 바로 불린다(큐 없음). 카운터 합은 C++ `MetricCounter` 처럼 칸 16개에 나눠 더한다(`Thread.GetCurrentProcessorId`, 2026-11-28 사례 16).

C++에 없는 이유: C++ 은 `MetricCounter`·`LatencyHistogram` 에 직접 넣으므로 중간에 모으는 곳이 필요 없다.

### 코덱(`IByteCodec<T>`, `ByteCodecs.cs`)

C++ 은 `operator<<` 하나 안의 `if constexpr` 로 타입을 가른다. C# 에는 컴파일 시간 타입 분기가 없어서, 타입마다 "쓰기·읽기" 묶음(코덱, codec: 인코더 + 디코더)을 `struct` 로 두고 static abstract 인터페이스로 묶는다. JIT(Just-In-Time 컴파일러)가 struct 코덱마다 코드를 따로 만들어 호출이 인라인된다(성능 우선). 코덱끼리 조합해 중첩 컨테이너도 쓴다.

| C++ | C# 코덱 |
|---|---|
| 정수, enum, bool, float/double, string | `IntegerCodec<T>`, `EnumCodec<T>`, `BoolCodec`, `SingleCodec`, `DoubleCodec`, `StringCodec` |
| `std::optional<T>` | `NullableCodec<T, C>`(값 형식 `int?`), `NullableRefCodec<T, C>`(참조 형식 `string?`) |
| `std::vector<T>` | `ArrayCodec<T, C>`(`T[]`), `ListCodec<T, C>`(`List<T>`), 정수 배열은 `IntegerArrayCodec<T>`(통째 복사) |
| `std::map<K, V>` | `DictionaryCodec<K, V, KC, VC>`(`Dictionary<K, V>`). 쓰는 순서는 Dictionary 순서(C++ map 은 키 순). 읽을 때 같은 키는 먼저 온 값 유지 |
| 클래스 `SERIALIZE_FIELDS(a, b)` | 클래스 + 코덱 struct 쌍(`Player` + `PlayerCodec`). 코덱을 struct 로 따로 두는 이유는 성능(참조 형식 코덱은 JIT 가 공유 코드로 만들어 인라인 안 됨). CodeGen 이 이 모양을 만든다 |
