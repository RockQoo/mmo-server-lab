#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

#include "Base/CrashDump.h"

// 살아 있는 서버 덤프(CrashDump::Write) 동안 서버가 얼마나 멈추나. 결과는 docs/journal/performance.html 사례 14.
//
// 게임 스레드 자리: 쉬지 않고 시각을 읽는 스레드 하나. 덤프가 프로세스를 멈추면 두 번 읽는 사이가 벌어진다. 덤프
// 동안 가장 크게 벌어진 간격 = 서버가 멈춘 시간(pause_ms). 덤프를 부른 스레드가 기다린 시간은 write_ms.
// 프로세스 메모리: 실제 서버처럼 큰 힙을 만들려고 HeapMB 만큼 잡아 모든 페이지를 쓴다(Full 덤프 크기가 이만큼 는다).
// 인자: {힙 MB, 방법(0 = Snapshot, 1 = Direct)}. Full 덤프 파일은 임시 폴더에 쓰고 바로 지운다.
namespace
{
    // ticker 가 count 바퀴 더 돌 때까지 기다린다
    void WaitLaps(const std::atomic<uint64_t>& laps, const uint64_t count)
    {
        const auto target = laps.load(std::memory_order_acquire) + count;
        while (laps.load(std::memory_order_acquire) < target)
        {
            std::this_thread::yield();
        }
    }

    void LiveFullDump(benchmark::State& state)
    {
        const auto heapBytes = static_cast<std::size_t>(state.range(0)) * 1024 * 1024;
        const auto method = state.range(1) == 0 ? Base::DumpMethod::Snapshot : Base::DumpMethod::Direct;

        static const auto Installed = []
        {
            Base::CrashDump::Install(
                {.directory = std::filesystem::temp_directory_path() / "mmo-server-lab-CrashDumpBench"});
            return true;
        }();
        static_cast<void>(Installed);

        const auto heap = std::make_unique_for_overwrite<std::byte[]>(heapBytes);
        for (std::size_t i = 0; i < heapBytes; i += 4096)
        {
            heap[i] = std::byte{1}; // 페이지마다 한 번 써서 실제 메모리로 만든다
        }

        std::atomic<bool> running{true};
        std::atomic<int64_t> maxGapNs{0};
        std::atomic<bool> measuring{false};
        std::atomic<uint64_t> laps{0}; // ticker 가 한 바퀴 돌 때마다 +1
        std::jthread ticker{[&]
                            {
                                auto last = std::chrono::steady_clock::now();
                                while (running.load(std::memory_order_relaxed))
                                {
                                    const auto now = std::chrono::steady_clock::now();
                                    if (measuring.load(std::memory_order_relaxed))
                                    {
                                        const auto gap =
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
                                        if (gap > maxGapNs.load(std::memory_order_relaxed))
                                        {
                                            maxGapNs.store(gap, std::memory_order_relaxed);
                                        }
                                    }
                                    last = now;
                                    laps.fetch_add(1, std::memory_order_release);
                                }
                            }};

        auto pauseMs = 0.0;
        auto fileMb = 0.0;
        for (auto iteration : state)
        {
            maxGapNs.store(0, std::memory_order_relaxed);
            measuring.store(true, std::memory_order_relaxed);
            WaitLaps(laps, 2); // ticker 가 measuring 을 본 바퀴가 지나간 뒤 시작
            const auto start = std::chrono::steady_clock::now();
            const auto path = Base::CrashDump::Write(Base::DumpKind::Full, "bench", method);
            const auto end = std::chrono::steady_clock::now();
            // 덤프가 끝나 풀려난 ticker 가 멈춘 간격을 기록할 때까지 기다린 뒤 닫는다(Codex 리뷰 R3: 바로 닫으면 Direct
            // 의 마지막 멈춤을 놓친다)
            WaitLaps(laps, 2);
            measuring.store(false, std::memory_order_relaxed);
            state.SetIterationTime(std::chrono::duration<double>(end - start).count());
            pauseMs = std::max(pauseMs, static_cast<double>(maxGapNs.load(std::memory_order_relaxed)) / 1e6);
            if (!path.empty())
            {
                fileMb = static_cast<double>(std::filesystem::file_size(path)) / (1024.0 * 1024.0);
                std::filesystem::remove(path);
            }
        }
        running.store(false, std::memory_order_relaxed);
        state.counters["pause_ms"] = pauseMs; // 반복 중 가장 긴 멈춤
        state.counters["file_mb"] = fileMb;
        benchmark::DoNotOptimize(heap.get());
    }
} // namespace

BENCHMARK(LiveFullDump)
    ->Name("CrashDump/LiveFull")
    ->ArgNames({"heap_mb", "direct"})
    ->Args({256, 0})
    ->Args({256, 1})
    ->Args({1024, 0})
    ->Args({1024, 1})
    ->Iterations(3)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
