# mmo-server-lab

MMORPG 서버를 기능 하나씩 만들면서, **설계 결정마다 벤치마크로 근거를 남기는** 포트폴리오다. 서버 구조는 하나(분산 존, `docs/decisions/001-서버-구조.md`)이고, **C++(주력)과 C#으로 같은 서버를 같이 만들어 비교한다.** 저장소 하나에 폴더로 나눈다. public 저장소로 공개한다(2026-11-23 결정).

## 기술 선택 (2026-11-23 개편)

| 항목 | 선택 |
|---|---|
| 서버 구조 | 분산 존: Gateway + ZoneServer (이후 Coordinator, ServiceServer, DbWriter) |
| C++ 서버 (주력) | C++23(MSVC `/std:c++latest`), Windows IOCP 직접 사용, CMake + vcpkg, GoogleTest. 메시지 큐 + 스레드 어피니티 |
| C# 서버 (비교) | .NET 10 (LTS). async/await, `Channel<T>`, GC 할당 최소화. xUnit, BenchmarkDotNet |
| 프로토콜 | `proto/` 스키마 하나 → `tools/CodeGen`(C# 콘솔)이 C++·C# 코드를 생성 |
| 클라이언트 | **UE5 5.8 하나**(C++만, 블루프린트 금지). 이동·액션 동기화 확인용 뷰어(UI, 이펙트, 지형 없음). 같은 프로토콜이라 두 서버에 다 붙는다 |
| 봇 | **C++ BotServer 하나**. 두 서버를 같은 봇·같은 시나리오로 잰다. Behavior Tree |
| DB | MSSQL Developer Edition (C++은 ODBC) |

이유는 `docs/decisions/`에 있다.

## 두 언어로 같이 만드는 규칙

- **C#은 C++의 포팅이 아니다(2026-11-23).** 같은 서버 구조를 C++은 C++다운 서버로, C#은 C#다운 서버로 각자 작성한다. C++ 코드를 C#으로 옮겨 적지 않는다.
- **같은 주제는 C++ 먼저, 바로 이어서 C#.** 단위 하나 = 같은 주제의 C++판 + C#판.
- **같은 개념이면 클래스 이름과 namespace가 같다**(`Base::ByteWriter` ↔ `Base.ByteWriter`). 대응표는 `docs/naming.md`. 이름이 표준 라이브러리·BCL과 충돌하면 C++ 쪽 이름을 바꿔 둘 다 겹치지 않게 한다.
- **한쪽 언어에만 필요한 것은 그쪽에만 둔다.** 예: C++ `RwLocked<T>`는 C#에 대응 클래스가 없다(이유는 `docs/naming.md`). 빠진 이유는 단위 보고서 비교표에 적는다.
- 파일 이름, 메서드 표기 같은 세부는 각 언어 관례를 따른다.

## 가져다 쓸 것과 직접 만들 것 (2026-11-24 갱신)

우선순위: **안전 > 성능 > 편의.** 서버는 성능을 우선하되, 안전(검사·실패 표시)은 빼지 않는다.

- **가져다 쓰는 후보는 두 종류뿐이다.** ① MS·표준 공식 제공: BCL(Base Class Library: .NET 기본 제공, `System.*`)의 `IBufferWriter<T>`, `BinaryPrimitives`, `Channel<T>`, `System.IO.Pipelines`, `ArrayPool<T>` 등과 C++ 표준 라이브러리. 문서와 답변에서 용어는 "원래 용어(풀이)" 형식으로 적는다. ② 아주 대중적인 외부 라이브러리(예: C# Dapper, C++ Boost, JSON nlohmann/json, enum 리플렉션 magic_enum).
- **후보가 있어도 성능이 나쁘면 직접 만든다.** 성능이 갈리는 자리(패킷 경로, 틱, 메모리)는 벤치마크로 비교하고 결과를 `docs/decisions/`에 남긴다.
- **외부 라이브러리를 새로 설치(vcpkg, NuGet)할 때는 먼저 사용자에게 알린다.** 무엇을, 왜, 대안은 무엇인지 말하고 확인받는다. 설치한 것은 `docs/libraries.md`에 적는다.
- 예: 직렬화는 직접 만든 형식(C++ `ByteWriter`)을 유지한다. MS 공식에는 C++과 같은 바이트를 내는 고성능 바이너리 직렬화가 없고(`BinaryFormatter`는 제거, JSON은 느림), 가장 빠른 MemoryPack은 C++ 구현이 없으며, Protobuf는 가변 길이 정수와 C++ 할당 비용이 있다. 나중에 직접 만든 형식 vs MemoryPack vs Protobuf 벤치마크로 근거를 남긴다.

## 프로토콜과 CodeGen

- 프로토콜이 정해지면 **반드시 CodeGen을 먼저 돌리고, 생성된 코드 위에서 개발한다.**
- 생성 파일은 **손으로 고치지 않는다.** 버그가 나면 CodeGen을 고치고 다시 생성한 뒤 이어서 개발한다. 생성 파일 맨 위에 "자동 생성, 직접 수정 금지"를 넣는다.
- 생성 파일은 커밋한다(리뷰에서 보이고, UE 빌드가 CodeGen 실행에 기대지 않는다). QA에서 다시 생성해 커밋본과 같은지 확인한다.
- 출력 위치는 세 곳이다: `server-cpp`(Base 직렬화 사용), `server-cs`(C# 직렬화), `client-ue5`(직렬화까지 포함한 독립 코드, C++20에서 컴파일되게. UE가 `server-cpp/Core`를 직접 참조하지 않는다).

## 폴더 (계획, 필요할 때 만든다)

| 경로 | 내용 |
|---|---|
| `proto/` | 프로토콜 스키마 원본 (하나) |
| `tools/CodeGen/` | C# 콘솔. proto → C++·C# 코드 생성 |
| `server-cpp/Core/` | C++ 서버 코어. 기술마다 정적 라이브러리 하나(`Base/`, `Messaging/`, `Network/`...), 폴더 이름 = namespace. .h와 .cpp를 같은 폴더에 둔다. include는 `Core/` 기준(`#include "Base/ByteWriter.h"`). 게임을 모른다. 목록은 `PROGRESS.md` |
| `server-cpp/Core/<Lib>/Tests/` | 라이브러리별 단위 테스트(gtest), 실행 파일 `<Lib>Tests` (예: `BaseTests`) |
| `server-cpp/Gateway/` | 클라 소켓, 세션별 경로표, 세션별 묶음 전송 |
| `server-cpp/ZoneServer/` | 전투·이동. 프로세스 하나에 존 여러 개, 존마다 전용 스레드와 틱 |
| `server-cpp/ZoneServer/Tests/` | 순수 계산 함수(이동 검증, AOI 셀, 범위 판정, 데미지, 확률)만 테스트. 계산 함수와 상태를 바꾸는 코드를 나눠 둔다. 난수는 인자로 받는다 |
| `server-cpp/Coordinator/` 등 | 나중에: `Coordinator/`(등록·배치·조회), `ServiceServer/`(길드·파티), `DbWriter/` |
| `server-cpp/BotServer/` | C++ 봇 서버. Core를 공유하고 Gateway에 붙는다 |
| `server-cpp/Bench/` | C++ 마이크로 벤치마크 (Google Benchmark, Core 링크) |
| `server-cpp/Tools/` | 확인용 도구 (EchoClient 등) |
| `server-cs/` | C# 서버(.slnx). 안은 `server-cpp/`와 같은 모양: `Core/<Lib>/`(+`Tests/`), `Gateway/`, `ZoneServer/`, `Bench/` |
| `client-ue5/` | UE5 5.8 클라이언트(뷰어). Starter Content 없이. 바이너리 에셋은 Git LFS(첫 에셋 때 설정) |
| `bench/` | 봇 부하 시나리오와 결과 원본(C++/C# 서버 공통) |
| `testdata/` | 두 언어 테스트가 같이 쓰는 정답 파일(`bytes/*.hex`: 같은 값 → 같은 바이트) |
| `docs/decisions/` | 결정 기록 `NNN-제목.md` |
| `docs/units/` | 단위별 보고서 HTML `NNN-주제.html`: 흐름도, 클래스 설명, C++/C# 비교표 (공개) |
| `docs/naming.md` | C++ ↔ C# 이름 대응표 |

의존 방향은 `서버 → Core`만 있다. 서버 실행 파일끼리 서로 링크하지 않는다(통신만 한다).

이름, 중괄호, 폴더 규칙은 `docs/coding-style.md`. 최상위는 소문자, `server-cpp/`·`server-cs/` 안은 전부 Pascal. 진행 목록은 `PROGRESS.md`.

## 빌드와 테스트

```powershell
powershell -File server-cpp/build.ps1 -Config debug          # 빌드 (release도 가능)
powershell -File server-cpp/build.ps1 -Config release -Test  # 빌드 + 단위 테스트 (qa.json이 쓴다)
```

- VS 개발자 환경을 켜고 Ninja 프리셋(`server-cpp/CMakePresets.json`)으로 빌드한다. 결과는 `server-cpp/build/<config>/`.
- 외부 라이브러리는 `server-cpp/vcpkg.json`(manifest, VS 포함 vcpkg)에 추가한다.
- `build.ps1`은 UTF-8 **BOM**으로 저장한다(Windows PowerShell 5.1이 BOM 없는 한글을 깨뜨림).
- **Visual Studio**: 루트의 `OpenVS.bat`(= `build.ps1 -Sln`)이 `server-cpp/build/vs/mmo_server_lab.slnx`를 만들고 연다. 솔루션 폴더는 Core / Servers / Tests(CMake 기본 대상은 `_CMake`), 시작 프로젝트는 ZoneServer. 대상마다 `FOLDER` 속성을 주고 헤더도 소스 목록에 적는다. 명령줄 빌드와 QA는 Ninja 프리셋 그대로.
- **C#**: `powershell -File server-cs/build.ps1 -Config debug|release [-Test]` (`dotnet build/test`, 솔루션 `server-cs/mmo_server_lab.slnx`, VS에서 바로 열린다). 공통 설정은 `server-cs/Directory.Build.props`(net10.0, nullable 경고는 에러). 테스트는 xUnit v3 + Microsoft.Testing.Platform(`global.json`). 출력 언어는 영어로 고정(qa.json 정규식).
- `qa.json`은 C++과 C# 빌드·테스트를 둘 다 돌린다. 코어 기본 QA는 두 언어 모두 기능으로 만들어 둔다.

## 진행 방식 (가장 중요)

- **코드를 조금씩만 쓴다.** 한 번에 클래스나 함수 하나 정도(대략 100~150줄, 파일 1~3개). 목표는 사용자가 코드를 이해하면서 같이 설계하는 것이다.
- 만들기 전에 무엇을 왜 만드는지, 선택지가 무엇인지 말하고, 고르는 건 가능하면 사용자에게 묻는다. 클래스 이름은 대중적인 후보를 먼저 제안한다.
- 단위 하나의 순서: **C++ 구현 → C# 구현 → 로컬 커밋 → Codex 리뷰 → 동의한 지적 수정(커밋) → 수정 커밋을 다시 리뷰 → … 지적이 0건이 되거나 남은 지적이 모두 반대(`code-review/`에 근거)일 때까지 반복 → `docs/units/` HTML 보고서 → 사용자 확인 → 보고서 커밋 → 다음 단위.** HTML 확인이 멈추는 지점이다.
  - 리뷰 스크립트는 커밋된 변경만 보므로 리뷰 전에 커밋한다(`--base <이전 커밋>`으로 그 단위만).
  - 리뷰마다 C++·C# 변경이 모두 범위에 들어갔는지 파일 목록으로 확인하고, 보고서에 차수별 범위(커밋, 파일 수)와 결과를 적는다.
- **단위 보고서(`docs/units/NNN-주제.html`)는 커밋해서 개발 기록으로 남긴다.** 나중에 설계 이유를 찾아볼 자료다. 고치지 않고 쌓는다(틀린 내용은 다음 보고서에서 바로잡는다).
- 코드 흐름은 review-tool(Projects/review-tool)로 본다. HTML 보고서는 코드 탐색을 다시 그리지 않고 설계 흐름, 클래스 설명, C++/C# 비교에 집중한다.
- "시작하자"는 한꺼번에 만들라는 뜻이 아니다.

## 공개 저장소 주의

- 이전 회사나 다른 게임의 이름, 내부 클래스 이름, 코드를 쓰지 않는다. 구조를 비교할 때는 일반 이름(홈 서버형, 동기 커밋 채널형 등)만 쓴다.
- 참고한 사람·프로젝트 이름을 적지 않는다(라이브러리 LICENSE는 유지).
- 비밀값, 내 PC 경로를 커밋하지 않는다.

## 결정 기록 형식

벤치마크나 설계 선택마다 `docs/decisions/NNN-제목.md` 하나.

- 질문 / 가설 / 조건(부하, 하드웨어) / 결과(표, 그래프) / 결론 / Trade-off(얻은 것, 잃은 것, 결론이 깨지는 조건)
- 측정 도구: 함수 단위는 Google Benchmark·BenchmarkDotNet, 전체 부하는 봇으로 실제 소켓을 붙여 잰다.

## push 순서

Claude 코드 작성 → Codex 리뷰(`review` 스킬) → 지적 검토(동의한 것 수정, 반대한 것은 `code-review/<날짜>.html`) → 기본 QA(`qa` 스킬, `qa.json`) → 사용자 승인 후 master push.

- `STATUS.md`, `QA-LOG.md`, `PERF-LOG.md`, `.qa/`, `.review/`, `code-review/`는 로컬 전용(`.gitignore`).
- 커밋은 단위마다 로컬까지만. GitHub 원격 저장소 설정과 push는 매번 확인받는다.
