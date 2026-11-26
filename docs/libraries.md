# 외부 라이브러리

대중적으로 많이 쓰는 라이브러리는 직접 만들지 않고 가져다 쓴다. 새로 추가할 때는 이 표에 이유와 함께 적는다.

## C++ (`server-cpp/vcpkg.json`)

| 라이브러리 | 용도 | 추가한 날 | 이유 |
|---|---|---|---|
| GoogleTest | 단위 테스트 | 2026-11-17 | C++ 테스트의 사실상 표준 |
| Google Benchmark (`benchmark`) | 마이크로 벤치마크(`server-cpp/Bench`) | 2026-11-26 | C++ 마이크로 벤치마크의 사실상 표준. 반복 횟수·워밍업·스레드별 측정과 합산을 처리한다 |

## C# (NuGet)

| 패키지 | 용도 | 추가한 날 | 이유 |
|---|---|---|---|
| xunit.v3, xunit.runner.visualstudio | 단위 테스트 | 2026-11-23 | .NET 테스트에서 가장 많이 쓰는 프레임워크. VS 테스트 탐색기 연동 |
| Microsoft.NET.Test.Sdk | 테스트 실행 | 2026-11-23 | `dotnet test`와 VS가 테스트를 찾고 실행하는 데 필요 |
| BenchmarkDotNet | 마이크로 벤치마크(`server-cs/Bench`) | 2026-11-26 | .NET 벤치마크의 사실상 표준(.NET 런타임 팀도 사용). JIT 워밍업·GC 영향을 걸러 내고 할당량(MemoryDiagnoser)을 잰다 |
