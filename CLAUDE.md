# mmo-server-lab

C++ IOCP MMORPG 서버를 기능 하나씩 만들면서, **설계 결정마다 벤치마크로 근거를 남기는** 포트폴리오다. 나중에 같은 설계를 C# 서버로 다시 만들어 비교한다. 공개 저장소로 올릴 예정이다(공개 포트폴리오 1개).

## 기술 선택

| 항목 | 선택 |
|---|---|
| 서버 | C++20, Windows IOCP 직접 사용, CMake + vcpkg |
| DB | MSSQL Developer Edition (C++은 ODBC) |
| 프로토콜 | 언어 중립 스키마 하나를 C++/C#이 공유 |
| 봇 | **C++ BotServer**(2026-11-18 변경, 처음엔 C#). Core를 Game과 공유하고, Game과 서로 통신한다. Behavior Tree |
| 뷰어 | Godot .NET 3D. 이동·액션 동기화 확인용(UI, 이펙트, 지형 없음) |

이유는 `docs/decisions/`에 있다.

## 폴더 (계획, 필요할 때 만든다)

| 경로 | 내용 |
|---|---|
| `proto/` | 패킷 스키마 (공유) |
| `server-cpp/Core/` | 서버 코어, 정적 라이브러리. 분류 폴더(`Util/`, `Network/`, `Buffer/`...)에 .h와 .cpp를 같이 둔다. 게임을 모른다 |
| `server-cpp/Core/Tests/` | Core 단위 테스트 |
| `server-cpp/GameServer/` | 게임 서버 실행 파일. Core를 링크해서 붙인다. 의존 방향은 `GameServer → Core`만 |
| `server-cpp/GameServer/Tests/` | GameServer의 순수 계산 함수(이동 검증, AOI 셀, 범위 판정, 데미지, 확률)만 테스트. 계산 함수와 상태를 바꾸는 코드를 나눠 둔다. 난수는 인자로 받는다 |
| `server-cpp/Bench/` | C++ 마이크로 벤치마크 (Google Benchmark, Core 링크) |
| `server-cpp/BotServer/` | C++ 봇 서버 실행 파일. Core를 GameServer와 공유하고 GameServer와 통신한다 |
| `server-cpp/Tools/` | 확인용 도구 (EchoClient 등) |
| `server-csharp/` | C# 서버 (나중에) |
| `viewer/` | Godot 뷰어 |
| `bench/` | 봇 부하 시나리오와 결과 원본(C++/C# 서버 공통) |
| `docs/decisions/` | 결정 기록 `NNN-제목.md` |

이름, 중괄호, 폴더 규칙은 `docs/coding-style.md`. 최상위는 소문자, `server-cpp/` 안은 전부 Pascal. 진행 목록은 `PROGRESS.md`.

## 빌드와 테스트

```powershell
powershell -File server-cpp/build.ps1 -Config debug          # 빌드 (release도 가능)
powershell -File server-cpp/build.ps1 -Config release -Test  # 빌드 + 단위 테스트 (qa.json이 쓴다)
```

- VS 개발자 환경을 켜고 Ninja 프리셋(`server-cpp/CMakePresets.json`)으로 빌드한다. 결과는 `server-cpp/build/<config>/`.
- 외부 라이브러리는 `server-cpp/vcpkg.json`(manifest, VS 포함 vcpkg)에 추가한다.
- `build.ps1`은 UTF-8 **BOM**으로 저장한다(Windows PowerShell 5.1이 BOM 없는 한글을 깨뜨림).
- Visual Studio에서는 `server-cpp/` 폴더를 열면 같은 프리셋을 쓴다.

## 진행 방식 (가장 중요)

- **코드를 조금씩만 쓴다.** 한 번에 클래스나 함수 하나 정도(대략 100~150줄, 파일 1~3개). 목표는 사용자가 코드를 이해하면서 같이 설계하는 것이다.
- 만들기 전에 무엇을 왜 만드는지, 선택지가 무엇인지 말하고, 고르는 건 가능하면 사용자에게 묻는다.
- 만든 뒤 핵심 줄을 짚어 주고 **멈춘다.** 다음 조각은 사용자 확인 뒤에 시작한다.
- "시작하자"는 한꺼번에 만들라는 뜻이 아니다.

## 결정 기록 형식

벤치마크나 설계 선택마다 `docs/decisions/NNN-제목.md` 하나.

- 질문 / 가설 / 조건(부하, 하드웨어) / 결과(표, 그래프) / 결론 / Trade-off(얻은 것, 잃은 것, 결론이 깨지는 조건)
- 측정 도구: 함수 단위는 Google Benchmark, 전체 부하는 봇으로 실제 소켓을 붙여 잰다.

## push 순서

Claude 코드 작성 → Codex 리뷰(`review` 스킬) → 지적 검토(동의한 것 수정, 반대한 것은 `code-review/<날짜>.html`) → 기본 QA(`qa` 스킬, `qa.json`) → 사용자 승인 후 master push.

- `STATUS.md`, `QA-LOG.md`, `PERF-LOG.md`, `.qa/`, `.review/`, `code-review/`는 로컬 전용(`.gitignore`).
- 커밋은 로컬까지만. GitHub 원격 저장소 생성과 push는 매번 확인받는다.
