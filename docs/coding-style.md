# 코딩 규칙 (C++)

개발하면서 하나씩 정한다. 정한 날짜를 함께 적는다.

## 자동 정리 (2026-11-17)

저장소 최상위 `.clang-format`이 아래 규칙을 맞춘다. VS는 Ctrl+K, Ctrl+D(문서 서식), VS Code는 저장할 때 적용된다. 도구가 원치 않게 바꾸는 부분은 `// clang-format off` ... `// clang-format on`으로 감싼다.

- 공백 4칸(탭 없음), 한 줄 최대 120자
- 포인터·참조는 타입 쪽: `int* p`, `const std::string& s`
- 짧은 if, 반복문, 함수, case도 한 줄로 합치지 않는다
- include는 빈 줄로 나눈 그룹(표준 / 외부 / 프로젝트) 안에서만 정렬
- namespace 끝에 `} // namespace Core` 주석이 자동으로 붙는다

## 확정

### 중괄호와 들여쓰기 (2026-11-17)

- 여는 중괄호는 **항상 다음 줄**에 둔다(Allman). namespace, class, 함수, if, for, while, switch 모두 같다.
- namespace 안도 **한 단계 들여쓴다.**

```cpp
namespace Core
{
    class RecvBuffer
    {
    public:
        void Clean();
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
| namespace | Pascal | `Base`, `Messaging`, `GameServer` |
| 클래스, 구조체, enum | Pascal | `RecvBuffer`, `SessionState` |
| enum 값 | Pascal, `enum class`만 사용 | `SessionState::Connected` |
| 함수, 멤버 함수 | Pascal | `BuildInfo()`, `OnRecv()` |
| 멤버 변수 | `_` + camel | `_readPos` |
| 지역 변수, 매개변수 | camel | `bytesTransferred` |
| 상수, constexpr | Pascal | `MaxPacketSize` |
| 매크로 | 대문자 + `_`, 되도록 쓰지 않음 | `ASSERT_CRASH` |
| 파일 | 클래스 이름과 같게, .h와 .cpp는 같은 폴더 | `Util/BuildInfo.h`, `Util/BuildInfo.cpp` |

### 폴더 (2026-11-17)

- 저장소 최상위는 소문자(GitHub 관습): `server-cpp/`, `viewer/`, `docs/`
- `server-cpp/` 안은 **전부 Pascal**(게임 서버 관습): `Core/Network/`, `GameServer/World/`, `Core/Tests/`, `Bench/`, `Tools/`
- include는 `server-cpp/` 기준 경로로 쓴다: `#include "Core/Util/BuildInfo.h"` (경로 = namespace)
- include/src 분리는 하지 않는다. 그래서 Game이 Core의 어떤 헤더든 include할 수 있다. **Game은 Core 내부 구현에 기대지 않는다**는 것은 규칙으로 지킨다(빌드로 강제하지 않음).
