# C++ ↔ C# 이름 대응표

같은 개념이면 클래스 이름과 namespace를 같게 한다. 한쪽에만 있는 것은 이유를 적는다. 구현이 끝난 것만 "있음"으로 적는다.

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
| Base | `RwLocked<T>` | 예정 | **없음** | 아래 참고 |

## 한쪽에만 있는 것

### `RwLocked<T>` (C++만)

데이터와 읽기/쓰기 락을 묶어, 락 없이는 값에 접근할 수 없게 하는 래퍼(`Read()`, `Write()`). 스레드 어피니티로 막지 못하는 자리에서만 쓴다.

C#에 만들지 않는 이유:

- C# 서버는 공유 상태를 존마다 `Channel<T>` 하나와 소비자 하나로 처리해 락이 필요 없게 설계한다.
- `lock`, `ReaderWriterLockSlim`은 스레드에 묶여 있어 `await`를 사이에 두고 쥘 수 없다(`await` 뒤 다른 스레드에서 이어질 수 있다).
- 재진입·승급 검사는 `ReaderWriterLockSlim(LockRecursionPolicy.SupportsRecursion)`이 이미 제공한다. 꼭 필요한 동기 락은 `System.Threading.Lock`, 비동기 구간은 `SemaphoreSlim`.

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
