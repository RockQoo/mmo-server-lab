# docs 목차

파일 이름은 영어 소문자에 `-`로 잇고(클래스 이름은 붙여서 한 단어: `bufferpool`), 내용은 한국어로 쓴다. 파일을 추가하면 여기에 한 줄 추가한다.

## 규칙과 대응표

| 파일 | 내용 |
|---|---|
| [coding-style.md](coding-style.md) | 코딩 규칙: 이름, 중괄호, 폴더, 정수 타입. 검사 도구 설정 |
| [naming.md](naming.md) | C++ ↔ C# 이름 대응표(확정된 이름만). 한쪽 언어에만 있는 클래스와 그 이유 |
| [class-map.html](class-map.html) | 클래스 지도: Core 전체 계획(앞으로 만들 것 포함), 진행 상태 |
| [libraries.md](libraries.md) | 가져다 쓴 외부 라이브러리와 이유 |

## 기록

| 폴더 | 내용 | 언제 쓰나 |
|---|---|---|
| [journal/](journal/) | 개발일지: 고민 과정(질문, 헷갈린 점, 대안, 그림 설명) | 만들기 전·중. 주제마다 파일 하나에 이어 쓴다 |
| [decisions/](decisions/) | 설계 결정 기록: 질문, 가설, 측정 결과, 결론, Trade-off | 설계를 정할 때. 고치지 않고 새 파일로 쌓는다 |
| [units/](units/) | 단위 보고서: 단위 하나(C++판 + C#판)를 끝낼 때의 완료 보고 | 단위를 끝냈을 때. 고치지 않고 쌓는다 |

흐름: 개발일지(고민) → 결정(측정과 결론) → 단위 보고서(완성).

### journal (개발일지)

| 파일 | 주제 |
|---|---|
| [performance.html](journal/performance.html) | **최적화·결정 노트(모음)**: 사례마다 증상 → 원인 → 해결 → 결과 + 그림, 면접용 30초 답변, 앞으로 할 최적화 계획. 공부·면접 준비는 여기부터 |
| [benchmark.html](journal/benchmark.html) | 벤치마크 측정 방법: 첫 측정에서 `benchmark::DoNotOptimize`(MSVC가 전역 변수에 씀)가 여러 스레드 결과를 왜곡한 일, true/false sharing, 벤치마크 확인 목록 |
| [bufferpool.html](journal/bufferpool.html) | BufferPool 1판(등급마다 mutex)이 왜 느린가, 2판(스레드별 캐시 + 전역 풀) 설계, 송신·수신 버퍼, 3판 후보(Lane 자체 풀) |
| [objectpool.html](journal/objectpool.html) | ObjectPool: BufferPool과의 관계(형제, 포함), 반납 때 소멸 vs `Reset()`, Reset을 한 줄로 못 쓰는 이유, C++ 풀 vs C# `DefaultObjectPool` vs `new` 측정 |
| [threading.html](journal/threading.html) | 스레드 구조(초안): 멈추는 성질로 나눈 IO·Logic·Blocking 풀, 데이터마다 주인 Lane 하나, 틱 = 존 Lane 메시지와 틱 끝 브로드캐스트, 입력이 몇 번 와도 같은 처리, DB 저장 두 가지(Fire-and-Forget·확정형)와 결과는 주인에게 Post, 금지 목록, 전용 틱·브로드캐스트 스레드와 비교 |
| [crashdump.html](journal/crashdump.html) | 덤프 분석: WinDbg(cdb)로 C++·C# 덤프를 열어 콜스택 확인, 심볼(pdb) 경로, release pdb가 없던 문제, 데드락 찾는 법(`~*kn`, `!syncblk`), 서버가 죽거나 멈췄을 때 순서 |
| [messagequeue.html](journal/messagequeue.html) | 메시지 큐: 객체 vs 직렬화한 바이트, 생산자 N → 소비자 M 측정 틀, N:M에서 C++ Lane이 C# Channel보다 느린 이유(가설), "C# Channel은 lock-free"를 소스로 바로잡은 일 |

### decisions (설계 결정)

| 파일 | 결정 |
|---|---|
| [001-server-architecture.md](decisions/001-server-architecture.md) | 서버 구조: 분산 존(Gateway + ZoneServer) |
| [002-message-queue-object-vs-bytes.md](decisions/002-message-queue-object-vs-bytes.md) | 메시지 큐에 객체 vs 직렬화한 바이트: 같은 프로세스 안은 객체(+ ObjectPool). N:M에서 C++ Lane이 병목, C# Channel이 목표선 |
| [003-lane-ordering-guarantee.md](decisions/003-lane-ordering-guarantee.md) | Lane의 보장: 같은 Lane에 넣은 일은 Post가 끝난 순서대로(넣은 스레드가 달라도). ConcurrentQueue 제외, 묶어 넣기는 조건부, 콘텐츠 규칙(길드 재료 구매 예) |
| [004-packet-flow-and-message-router.md](decisions/004-packet-flow-and-message-router.md) | 패킷 흐름(IOCP에서 역직렬화, 수신 단위로 묶어 넣기, PacketDispatcher), MessageRouter 사용법(템플릿 + enum, 컴파일 검사), 틱 위치 R&D |

### units (단위 보고서)

| 파일 | 단위 |
|---|---|
| [001-bytewriter.html](units/001-bytewriter.html) | Base 직렬화: ByteWriter / ByteReader |
| [002-csharp-serialization.html](units/002-csharp-serialization.html) | C# 직렬화: 코덱으로 컨테이너·nullable·Dictionary·클래스 |
| [003-assert-and-warnings.html](units/003-assert-and-warnings.html) | C# Assert, 경고를 가장 높게 + 경고 = 빌드 에러 |
| [004-messaging-lane.html](units/004-messaging-lane.html) | Messaging: Lane(일을 순서대로 처리하는 스레드), 코딩 규칙 검사 |
| [005-ruid.html](units/005-ruid.html) | Base: StrongId, Ruid(모든 서버에서 유일한 64비트 ID), RuidGenerator |
| [006-lanegroup.html](units/006-lanegroup.html) | Messaging: LaneGroup(키로 Lane 고르기) |
| [007-bufferpool.html](units/007-bufferpool.html) | Memory: BufferPool 1판(등급마다 mutex), PooledBuffer |
| [008-bufferpool-v2.html](units/008-bufferpool-v2.html) | Memory: BufferPool 2판(스레드 캐시 + 전역 묶음), 첫 벤치마크, 측정 도구·빌드 문제 |
| [009-objectpool.html](units/009-objectpool.html) | Memory: ObjectPool(반납 때 Reset, 스레드 캐시 + 전역 묶음), C++ 풀 vs C# DefaultObjectPool·new, 측정 틀 ThreadTeam |
