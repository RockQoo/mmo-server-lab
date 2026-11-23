# 코딩 규칙 (C++)

클래스·namespace 이름은 C#과 같다(`docs/naming.md`). 변수 이름 규칙도 C#과 같다(C#의 MS 공식 규칙에 C++을 맞췄다, 2026-11-24). C# 고유 규칙은 아래 "코딩 규칙 (C#)".

**검사(2026-11-24)**: 이름과 서식은 사람이 지키는 것이 아니라 도구가 검사한다. C++은 `.clang-format`(서식) + `.clang-tidy`(이름, 버그 검사), C#은 `.editorconfig`(이름, 스타일, 빌드 에러) + `dotnet format`. `qa.json`이 모두 돌린다.

개발하면서 하나씩 정한다. 정한 날짜를 함께 적는다.

## 자동 정리 (2026-11-17)

저장소 최상위 `.clang-format`이 아래 규칙을 맞춘다. VS는 Ctrl+K, Ctrl+D(문서 서식), VS Code는 저장할 때 적용된다. 도구가 원치 않게 바꾸는 부분은 `// clang-format off` ... `// clang-format on`으로 감싼다.

- 공백 4칸(탭 없음), 한 줄 최대 120자
- 포인터·참조는 타입 쪽: `int* p`, `const std::string& s`
- 짧은 if, 반복문, 함수, case도 한 줄로 합치지 않는다
- include는 빈 줄로 나눈 그룹(표준 / 외부 / 프로젝트) 안에서만 정렬
- namespace 끝에 `} // namespace Base` 주석이 자동으로 붙는다

## 확정

### 중괄호와 들여쓰기 (2026-11-17)

- 여는 중괄호는 **항상 다음 줄**에 둔다(Allman). namespace, class, 함수, if, for, while, switch 모두 같다.
- namespace 안도 **한 단계 들여쓴다.**

```cpp
namespace Base
{
    class RecvBuffer
    {
    public:
        void Clean();

    private:
        std::size_t _readPos{};
        std::size_t _writePos{};
    };

    void RecvBuffer::Clean()
    {
        if (_readPos == _writePos)
        {
            _readPos = _writePos = 0;
        }
    }
}
```

### switch (2026-11-17)

case는 switch보다 한 단계 들여쓴다.

```cpp
switch (state)
{
    case SessionState::Connected:
        OnConnected();
        break;
    default:
        break;
}
```

### 변수 이름: 범위를 접두사로 (2026-11-17 멤버 `_camel`, 2026-11-24 `s_`·`t_`·`g_` 추가)

변수가 어디에 사는지를 "글자 하나 + `_`"로 이름 앞에 적는다. 서버에서는 이름만 보고 **여러 스레드가 같이 보는 변수인지** 알 수 있어야 한다.

| 범위 | 접두사 | 예 | 스레드 |
|---|---|---|---|
| 멤버 변수(private·protected) | `_` + camel | `_readPos` | 객체 하나에 속한다. Lane처럼 한 스레드만 만지게 설계하는 단위 |
| static(클래스 static, 파일 안 static, 함수 안 static) | `s_` + camel | `s_instanceCount` | 프로세스에 하나. 모든 스레드가 같이 본다(락 필요) |
| 스레드별(`thread_local`) | `t_` + camel | `t_currentLane` | 스레드마다 따로. 락 필요 없음 |
| 전역 | `g_` + camel, 되도록 쓰지 않는다 | `g_config` | 모든 스레드가 같이 본다 |
| 지역 변수, 매개변수 | camel(접두사 없음) | `bytesTransferred` | |
| 상수(`constexpr`, 전역·static `const`) | Pascal(static이어도 접두사 없음). 지역 `const auto x` 는 지역 변수라 camel | `MaxCount` | 바뀌지 않아 같이 봐도 안전 |

- 데이터 구조체의 public 필드(패킷·저장 구조체)는 접두사 없이 camel: `id`, `name`.
- C# 의 MS 공식 규칙(`_camel`, `s_`, `t_`)과 같다. C++ 에서는 `m_`(MFC·Win32, 국내 게임 서버)가 더 흔하지만, 두 서버가 같은 모양이도록 C# 쪽에 맞췄다.
- `_` 다음은 **항상 소문자**. `_` + 대문자와 `__`는 C++ 표준이 예약한 이름이다(`_` + 소문자는 전역 범위에서만 예약이라 멤버에는 문제없다). 그래서 `_`로 시작하는 이름은 멤버에만 쓴다.

### 이름 (2026-11-17)

게임 서버 관습(Win32, Unreal, 국내 게임 서버)을 따른다.

| 대상 | 규칙 | 예 |
|---|---|---|
| namespace | Pascal | `Base`, `Messaging`, `ZoneServer` |
| 클래스, 구조체, enum | Pascal | `RecvBuffer`, `SessionState` |
| enum 값 | Pascal, `enum class`만 사용 | `SessionState::Connected` |
| 함수, 멤버 함수 | Pascal | `BuildInfo()`, `OnRecv()` |
| 변수 | 위 "변수 이름" 표 | `_readPos`, `s_count`, `t_currentLane`, `size` |
| 상수, constexpr | Pascal | `MaxPacketSize` |
| 매크로 | 대문자 + `_`, 되도록 쓰지 않음 | `ASSERT_CRASH` |
| 파일 | 클래스 이름과 같게, .h와 .cpp는 같은 폴더 | `Base/ByteWriter.h`, `Base/ByteWriter.cpp` |

### enum (2026-11-24)

- `enum class`에는 **0 값을 둔다**(보통 `None = 0`). `Job job{}`처럼 기본값으로 만든 값이 없는 값(0)이 되지 않게 한다. clang-tidy `bugprone-invalid-enum-default-initialization`과 C# CA1008이 같은 규칙이고, 두 언어 모두 검사한다.

### 폴더 (2026-11-17)

- 저장소 최상위는 소문자(GitHub 관습): `server-cpp/`, `server-cs/`, `client-ue5/`, `docs/`
- `server-cpp/`·`server-cs/` 안은 **전부 Pascal**(게임 서버 관습): `Core/Network/`, `ZoneServer/World/`, `Core/Base/Tests/`, `Bench/`, `Tools/`
- include는 `server-cpp/Core/` 기준 경로로 쓴다: `#include "Base/ByteWriter.h"` → `Base::ByteWriter` (경로 = namespace)
- include/src 분리는 하지 않는다. 그래서 Game이 Core의 어떤 헤더든 include할 수 있다. **Game은 Core 내부 구현에 기대지 않는다**는 것은 규칙으로 지킨다(빌드로 강제하지 않음).

### const, constexpr (2026-11-18)

쓸 수 있는 곳에는 **항상** 쓴다.

- 지역 변수, 값으로 받는 매개변수: `const auto size = ...`, `void Resize(const std::size_t size)`. 매개변수의 const는 선언(.h)과 정의(.cpp) 모두에 쓴다.
- 상태를 바꾸지 않는 멤버 함수: `std::size_t Size() const;`
- 컴파일 시간에 정해지는 값: `constexpr std::size_t MaxPacketSize{4096};`

붙이지 않는 곳:

| 자리 | 이유 |
|---|---|
| 멤버 변수 | const 멤버가 있으면 클래스가 대입·이동을 못 한다(컨테이너, 풀링에서 막힘) |
| 값으로 반환하는 타입 | `const std::string Get()`은 받는 쪽에서 move 대신 복사가 된다 |
| 나중에 move할 지역 변수 | const에서 `std::move`하면 에러 없이 복사된다 |

### 초기화 (2026-11-18)

- 기본은 **중괄호 초기화**: `bool _valid{true};`, `std::size_t _readPos{};`(값 초기화 = 0). 축소 변환(`int a{3.7}`, `std::uint8_t b{300}`)을 컴파일 에러로 막는다.
- `= 값` 형식(`bool _valid = true;`)은 대입이 아니라 복사 초기화다. 성능은 같지만 축소 변환을 막지 못하므로 쓰지 않는다.
- 멤버 변수는 **모두** 초기화를 붙인다: `std::vector<std::byte> _buffer{};`
- 예외: `std::initializer_list` 생성자가 있는 타입(컨테이너)에 크기나 개수를 줄 때는 `()`를 쓴다. `std::vector<int> v(3)`은 원소 3개, `v{3}`은 원소 1개(3)다.

### auto (2026-11-18)

쓸 수 있는 곳에는 **항상** 쓴다(AAA, Almost Always Auto).

```cpp
const auto size = _buffer.size();   // 타입 추론
auto raw = std::uint8_t{};          // 타입이 중요하면 오른쪽에 적는다
auto count = std::size_t{0};        // auto count = 0; 은 int가 된다
```

- `std::vector<bool>`처럼 프록시를 돌려주는 경우는 auto로 받으면 프록시가 남는다. 이럴 때만 타입을 적는다.

### std::move (2026-11-18)

적극적으로 쓴다. 특히 "받아서 보관하는" 매개변수는 값으로 받아 move한다.

```cpp
void Push(Message msg)
{
    _queue.push_back(std::move(msg));
}
```

- move한 변수는 다시 쓰지 않는다.
- `return std::move(local);`은 쓰지 않는다. 반환값 최적화(NRVO)를 막는다. `return local;`
- const 변수에는 쓰지 않는다(복사가 된다). 그래서 move할 지역 변수는 const를 붙이지 않는다.
- 기본형(int, 포인터)은 move가 곧 복사라 쓰지 않는다.

# 코딩 규칙 (C#)

클래스·namespace 이름은 C++과 같다(`docs/naming.md`). 나머지는 C# 관례(MS 공식 이름 규칙)를 따른다.

### 변수 이름 (2026-11-24)

| 범위 | 규칙 | 예 | C++ |
|---|---|---|---|
| private·internal 필드 | `_` + camel | `_pending` | 같음 |
| static 필드 | `s_` + camel | `s_instanceCount` | 같음 |
| 스레드별(`[ThreadStatic]`) | `t_` + camel | `t_currentLane` | 같음 |
| 지역 변수, 매개변수 | camel | `running` | 같음 |
| 상수(`const`), public 멤버, 메서드, 속성 | Pascal | `MaxCount` | 같음 |

`.editorconfig`의 이름 규칙이 검사하고, 어기면 빌드 에러다(IDE1006).

### 검사(Assert) (2026-11-24)

C++ `ASSERT` / `DEBUG_ASSERT`를 따로 만들지 않고 BCL(Base Class Library: .NET 기본 제공)을 쓴다.

| 쓸 때 | C# | C++ |
|---|---|---|
| 기본. Debug·Release 모두 검사 | `Trace.Assert(조건, "메시지")` | `ASSERT(조건, "메시지")` |
| 비싼 검사만. Debug에서만 | `Debug.Assert(조건, "메시지")` | `DEBUG_ASSERT(조건, "메시지")` |

- 실패하면 메시지와 호출 스택(파일·줄)을 남기고 `Environment.FailFast`로 끝난다. try/catch로 잡을 수 없다(종료 코드 `0x80131623`).
- `Trace.Assert`는 `TRACE` 기호가 있어야 남는다. `server-cs/Directory.Build.props`에 명시했고, 빠지면 `AssertTest`가 깨진다.
- 받은 데이터 검사(패킷 길이 등)에는 쓰지 않는다. 그건 실패 표시(`IsValid`)로 처리한다. Assert는 "코드가 틀렸다"는 뜻이다.
