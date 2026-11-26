# mmo-server-lab

MMORPG 서버를 기능 하나씩 만들면서, **설계 결정마다 벤치마크로 근거를 남기는** 프로젝트입니다. 같은 서버 구조를 **C++과 C#으로 같이** 만들고, 같은 봇 부하로 비교합니다.

- 예: I/O 스레드를 1개에서 N개로 늘리면 처리량은 계속 좋아지는가? 어디서 포화되고, 왜 그런가?
- 예: 같은 존 틱을 C++(메시지 큐 + 스레드 어피니티)과 C#(async/await + Channel)으로 만들면 지연 p99와 GC 멈춤은 어떻게 다른가?
- 기술마다 얻은 것과 잃은 것(Trade-off)을 `docs/decisions/`에 기록합니다.

## 서버 구조

분산 존 구조입니다. 클라이언트는 Gateway에 붙고, Gateway가 세션별 경로표로 패킷을 ZoneServer에 넘깁니다. 존마다 전용 스레드와 틱이 있고, 존 이동(handoff) 코드를 그대로 서버 간 전쟁에 씁니다. 이유와 단계는 [docs/decisions/001-server-architecture.md](docs/decisions/001-server-architecture.md)에 있습니다.

## 구성

| 경로 | 내용 |
|---|---|
| `proto/` | 프로토콜 스키마 (하나) |
| `tools/CodeGen/` | 스키마 → C++·C# 코드 생성기 (C#) |
| `server-cpp/` | C++ 서버. `Core/`(기술별 정적 라이브러리, 각자 gtest), `Gateway/`, `ZoneServer/`, `BotServer/`, `Bench/` |
| `server-cs/` | C# 서버. 같은 구조, 같은 클래스 이름 |
| `client-ue5/` | UE5 클라이언트 (C++), 이동·액션 동기화 확인용 뷰어 |
| `bench/` | 봇 부하 시나리오와 결과 |
| `docs/decisions/` | 설계 결정 기록 |
| `docs/units/` | 작업 단위별 보고서 (흐름도, 클래스 설명, C++/C# 비교) |
| `docs/naming.md` | C++ ↔ C# 이름 대응표 |

## 기술

C++23 · Windows IOCP · CMake + vcpkg · GoogleTest · C# (.NET 10) · xUnit · BenchmarkDotNet · Unreal Engine 5 · MSSQL

## 진행 상황

진행 중입니다. Core 라이브러리를 하나씩 만들고 있습니다. 목록과 상태는 [PROGRESS.md](PROGRESS.md)에 있습니다.
