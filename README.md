# mmo-server-lab

C++ IOCP 기반 MMORPG 서버를 기능 하나씩 만들면서, **설계 결정마다 벤치마크로 근거를 남기는** 프로젝트입니다.

- 예: I/O 스레드를 1개에서 N개로 늘리면 처리량은 계속 좋아지는가? 어디서 포화되고, 왜 그런가?
- 기술마다 얻은 것과 잃은 것(Trade-off)을 `docs/decisions/`에 기록합니다.
- 같은 설계를 나중에 C# 서버로 다시 구현해, 같은 봇 부하로 비교합니다.

## 구성

| 경로 | 내용 |
|---|---|
| `server-cpp/core/` | 서버 코어 (IOCP 네트워크, 버퍼, 잡 큐, 타이머, DB) |
| `server-cpp/game/` | 게임 서버 (이동·전투 동기화, AOI 브로드캐스팅, 콘텐츠) |
| `server-cpp/bench/` | 마이크로 벤치마크 |
| `server-csharp/` | C# 서버 (예정) |
| `proto/` | 패킷 스키마 (C++/C# 공유) |
| `bot/` | Behavior Tree 봇 (C#), 부하 테스트와 사람 같은 플레이 |
| `viewer/` | Godot 3D 뷰어, 이동·액션 동기화 확인용 |
| `bench/` | 봇 부하 시나리오와 결과 |
| `docs/decisions/` | 설계 결정 기록 |

## 기술

C++20 · Windows IOCP · CMake + vcpkg · MSSQL (ODBC) · C# · Godot .NET

## 진행 상황

진행 중입니다. 0단계(골격)부터 단계별로 만들고 있습니다.
