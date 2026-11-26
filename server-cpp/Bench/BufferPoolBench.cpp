#include <benchmark/benchmark.h>

#include <cstddef>

#include "Memory/BufferPool.h"

// 버퍼 하나를 빌려 한 바이트 쓰고 바로 돌려주는 비용. 패킷 하나를 만들고 보내는 모양의 가장 짧은 경로다.
// 스레드 1·2·4·8개가 동시에 같은 일을 한다(Lane 여러 개가 풀 하나를 같이 쓰는 모양).
// 결과 해석: items_per_second 를 본다. 모든 스레드를 합친 초당 (빌리기 + 반납) 횟수다(Google Benchmark가 스레드 결과를
// 더한다). C# Bench/ArrayPoolBench 의 1 / Mean 과 같은 기준이다. 스레드가 늘어도 처리량이 늘지 않으면 경합이다.
//
// 비교 대상
//   NewDelete       풀 없이 매번 new[]/delete[](기준)
//   BufferPoolV1    1판: 등급마다 mutex 하나

namespace
{
    // 모든 스레드가 같은 풀을 쓴다. 프로그램이 끝날 때 사라진다(그때 빌려 간 버퍼는 없다).
    Memory::BufferPool& SharedPool()
    {
        static Memory::BufferPool s_pool;
        return s_pool;
    }

    void NewDelete(benchmark::State& state)
    {
        const auto size = static_cast<std::size_t>(state.range(0));
        for (auto iteration : state)
        {
            auto* data = new std::byte[size];
            data[0] = std::byte{1}; // 쓰지 않으면 컴파일러가 할당을 지울 수 있다
            benchmark::DoNotOptimize(data);
            delete[] data;
        }
        state.SetItemsProcessed(state.iterations());
    }

    void BufferPoolV1(benchmark::State& state)
    {
        const auto size = static_cast<std::size_t>(state.range(0));
        auto& pool = SharedPool();
        for (auto iteration : state)
        {
            const auto buffer = pool.Rent(size);
            buffer.Data()[0] = std::byte{1};
            benchmark::DoNotOptimize(buffer.Data());
        }
        state.SetItemsProcessed(state.iterations());
    }
} // namespace

// 크기: 256B(이동 패킷), 4KB(큰 패킷). 스레드: 1, 2, 4, 8.
BENCHMARK(NewDelete)->Arg(256)->Arg(4096)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BufferPoolV1)->Arg(256)->Arg(4096)->ThreadRange(1, 8)->UseRealTime();
