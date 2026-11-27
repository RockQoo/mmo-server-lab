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
| Base | `BuildInfo` | 있음(함수 `BuildInfo()`) | 있음(`BuildInfo.Describe()`) | C#은 자유 함수가 없어 정적 클래스. C++은 컴파일러·`__cplusplus`, C#은 런타임·GC 모드(Server/Workstation)를 남긴다 |
| Base | `ASSERT` / `DEBUG_ASSERT` | 있음(매크로, 직접 만듦) | BCL `Trace.Assert` / `Debug.Assert`(만들지 않음) | 같은 역할: 항상 검사 / Debug만 검사, 실패하면 기록 → 디버거면 멈춤 → 잡을 수 없는 종료. C++은 RaiseException(0xE000A55E), C#은 FailFast(0x80131623). 덤프는 C# 은 런타임 설정(`DOTNET_DbgEnableMiniDump=1`), C++ 은 나중에 CrashDump |
| Messaging | `Lane` | 있음(mutex + 두 버퍼 교체 큐, 전용 `std::jthread`, `Post`→bool, `Stop`) | 있음(BCL `Channel<Action>` + 스레드 풀 async 루프, `TryPost`→bool, `DisposeAsync`) | 같은 역할: 넣은 순서대로 한 번에 하나씩, 멈출 때 남은 일 처리, 멈춘 뒤 넣기 거부, 일이 던지면 프로세스 종료(C++ terminate / C# FailFast). C#은 스레드가 고정되지 않는다(쉬는 동안 스레드를 잡지 않음). bool 반환이라 C#은 관례대로 Try 접두사 |
| Messaging | `LaneGroup` | 있음(`LaneGroup(name, count)`, `LaneFor(key)`·StrongId, `At(i)`, 스레드 이름 `"name-i"`, 소멸자는 모두 멈춘 뒤 해제) | 있음(`new LaneGroup(count)`, `LaneFor(long)`, 인덱서 `[i]`, `DisposeAsync`) | 같은 역할: 같은 키 → 같은 Lane. 키는 splitmix64 마무리로 섞은 뒤 Lane 수로 나눔(두 언어가 같은 번호). C#은 스레드 풀이라 이름이 없다 |
| Base | `Ruid` | 있음(`class Ruid : public StrongId<RuidTag>`, `Ruid::Init`/`Ruid::Create`, 칸은 멤버 `id.Realm()`) | 있음(`readonly record struct Ruid`, `Ruid.Init`/`Ruid.Create`, 칸은 멤버 `id.Realm`, `RuidCodec`) | 모든 서버에서 유일한 64비트 ID: Sign 0, Timestamp 41(2026-01-01 기준 ms), Realm 7, Node 6, Sequence 9. 같은 칸이면 두 언어가 같은 정수. 0 = 없음(Realm 0 예약) |
| Base | `RuidGenerator` | 있음(`std::atomic` CAS, `Clock` 함수 주입, `alignas`) | 있음(`Interlocked.CompareExchange`, BCL `TimeProvider`, 패딩 구조체) | 같은 역할: (시각, 순번)을 원자 변수 하나에 담아 CAS, 단조 시계, 512개 소진 시 다음 ms까지 양보, `InitializeGlobal`/`Global` |
| Log | 로그 함수·`Logger` | 있음(namespace `Log`: `Log::Initialize(options, sinks)`, `Log::Info(카테고리 enum, "문장").KV(키, 값)`, `Shutdown`/`Flush`. `LogSink`·`ConsoleSink`·`FileSink`, 로그 전용 Lane) | 있음(`Log.Logger`: `Initialize(LogOptions, sinks)`, `For(카테고리 enum)` → Serilog `ILogger`, `Shutdown`) | 같은 역할: 레벨 6개(VRB~FTL), 카테고리 `[Battle]`, 키·값, 날짜별 파일(`zone-.log` → `zone-20261127.log`), 같은 줄 모양. C#은 Serilog(메시지 템플릿 `attacker={Attacker}`), 파일:줄 없음. C# 은 namespace `Log` 와 같은 이름의 클래스를 둘 수 없어(CA1724) `Logger`. `Serilog.Core` 를 쓰는 파일(sink 구현)에서는 `Serilog.Core.Logger` 와 겹쳐 `using Logger = Log.Logger;` 로 고른다 |
| Base | `StrongId<Tag>` | 있음(템플릿, `std::hash`·`std::formatter`·ByteWriter 지원) | **없음**(ID마다 `record struct`) | 아래 참고 |
| Base | `RwLocked<T>` | 예정 | **없음** | 아래 참고 |
| Memory | `BufferPool` / `PooledBuffer` | 있음(2판: 전역 하나 `Shared()`, 스레드 캐시 + 전역 묶음 이동, 크기 등급 5개 256B~64KB, RAII 반납 핸들) | **없음**(BCL `ArrayPool<byte>.Shared`) | 아래 참고 |
| Memory | `ObjectPool<T>` / `PooledPtr<T>` | 있음(템플릿, 타입마다 전역 하나 `Shared()`, 스레드 캐시 64 + 전역 묶음 32, 반납 때 `Reset()`, 핸들은 `unique_ptr` 별칭 8B) | **없음**(MS NuGet `Microsoft.Extensions.ObjectPool`의 `DefaultObjectPool<T>` + 정책 `Return`) | 같은 역할: 객체를 소멸시키지 않고 비워서 다시 쓴다. 아래 참고 |

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

## C# 에만 있는 것

### 코덱(`IByteCodec<T>`, `ByteCodecs.cs`)

C++ 은 `operator<<` 하나 안의 `if constexpr` 로 타입을 가른다. C# 에는 컴파일 시간 타입 분기가 없어서, 타입마다 "쓰기·읽기" 묶음(코덱, codec: 인코더 + 디코더)을 `struct` 로 두고 static abstract 인터페이스로 묶는다. JIT(Just-In-Time 컴파일러)가 struct 코덱마다 코드를 따로 만들어 호출이 인라인된다(성능 우선). 코덱끼리 조합해 중첩 컨테이너도 쓴다.

| C++ | C# 코덱 |
|---|---|
| 정수, enum, bool, float/double, string | `IntegerCodec<T>`, `EnumCodec<T>`, `BoolCodec`, `SingleCodec`, `DoubleCodec`, `StringCodec` |
| `std::optional<T>` | `NullableCodec<T, C>`(값 형식 `int?`), `NullableRefCodec<T, C>`(참조 형식 `string?`) |
| `std::vector<T>` | `ArrayCodec<T, C>`(`T[]`), `ListCodec<T, C>`(`List<T>`), 정수 배열은 `IntegerArrayCodec<T>`(통째 복사) |
| `std::map<K, V>` | `DictionaryCodec<K, V, KC, VC>`(`Dictionary<K, V>`). 쓰는 순서는 Dictionary 순서(C++ map 은 키 순). 읽을 때 같은 키는 먼저 온 값 유지 |
| 클래스 `SERIALIZE_FIELDS(a, b)` | 클래스 + 코덱 struct 쌍(`Player` + `PlayerCodec`). 코덱을 struct 로 따로 두는 이유는 성능(참조 형식 코덱은 JIT 가 공유 코드로 만들어 인라인 안 됨). CodeGen 이 이 모양을 만든다 |
