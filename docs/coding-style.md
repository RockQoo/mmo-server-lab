# 코딩 규칙 (C++)

C# 쪽도 이름(Pascal, `_camel` 멤버)과 namespace는 C++과 같게 한다(`docs/naming.md`). C# 고유 규칙은 C# 코드를 만들 때 이 파일에 추가한다.

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

### 멤버 변수 (2026-11-17)

- 앞에 `_`를 붙이고 그 뒤는 camelCase: `_readPos`, `_sessionId`
- `_` 다음은 **항상 소문자**. `_ReadPos`처럼 `_` + 대문자는 C++ 표준이 예약한 이름이라 금지. `__`도 금지.
- `_`는 멤버 변수에만 쓴다. 지역 변수, 전역, 함수에는 쓰지 않는다.
- C# 서버의 `_field` 관습과 같아서 두 서버에서 같은 이름을 쓸 수 있다.

### 이름 (2026-11-17)

게임 서버 관습(Win32, Unreal, 국내 게임 서버)을 따른다.

| 대상 | 규칙 | 예 |
|---|---|---|
| namespace | Pascal | `Base`, `Messaging`, `ZoneServer` |
| 클래스, 구조체, enum | Pascal | `RecvBuffer`, `SessionState` |
| enum 값 | Pascal, `enum class`만 사용 | `SessionState::Connected` |
| 함수, 멤버 함수 | Pascal | `BuildInfo()`, `OnRecv()` |
| 멤버 변수 | `_` + camel | `_readPos` |
| 지역 변수, 매개변수 | camel | `bytesTransferred` |
| 상수, constexpr | Pascal | `MaxPacketSize` |
| 매크로 | 대문자 + `_`, 되도록 쓰지 않음 | `ASSERT_CRASH` |
| 파일 | 클래스 이름과 같게, .h와 .cpp는 같은 폴더 | `Base/ByteWriter.h`, `Base/ByteWriter.cpp` |

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
