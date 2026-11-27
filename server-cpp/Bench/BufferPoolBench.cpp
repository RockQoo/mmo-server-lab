#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Memory/BufferPool.h"
#include "RentReturnGrid.h"

// 버퍼 풀 측정 두 가지. 결과는 docs/journal/bufferpool.html, docs/decisions/ 에 정리한다.
//
// 1) SameThread: 버퍼 하나를 빌려 한 바이트 쓰고 바로 돌려준다. 같은 스레드가 빌리고 반납한다.
// 2) RentReturn: 빌리는 스레드 N개(Lane이 송신 패킷을 만드는 쪽 등) → 반납하는 스레드 M개(IOCP 스레드가 송신 완료 후
//    버리는 쪽 등). 64개씩 블록으로 넘긴다. 틀은 Bench/RentReturnGrid.h(ThreadTeam, 쌍마다 우편함). 2026-11-27 예전
//    CrossThread(독립된 1:1 쌍)를 이것으로 바꿨다. 예전 수치는 docs/journal/bufferpool.html 에 남아 있다.
//
// 측정 방법(SameThread, Noop): 한 번 측정할 때마다 스레드 N개를 띄워 전체 TotalOps 번을 나눠 하고, 모두 끝날 때까지의
// 시간을 잰다(ThreadTeam 으로 옮기는 것은 PROGRESS 후순위).
//
// 결과 해석: items_per_second = 모든 스레드를 합친 초당 (빌리기 + 반납) 횟수. 크면 좋다. 스레드 시작 비용은
// TotalOps 가 커서 전체의 몇 % 안 된다. Noop(아무 일도 하지 않는 반복)은 측정 틀의 기준이다: Noop 이 스레드 수만큼
// 늘지 않으면 측정 틀에 공유 자원이 있다는 뜻이라 여러 스레드 결과를 믿지 않는다.
//
// DoNotOptimize: 스레드 안 반복에서는 benchmark::DoNotOptimize 를 쓰지 않는다. MSVC 용 구현은 값을 전역 변수 하나에
// 쓰므로, 모든 스레드가 매 반복 같은 캐시 라인에 써서 서로 느려진다(2026-11-26: Noop 이 8스레드에서 1스레드와 같았다.
// 표준 C++만으로 재면 8배로 늘었다). 대신 스레드마다 자기 volatile 변수에 쓴다(docs/journal/bufferpool.html).
//
// 비교 대상
//   NewDelete     풀 없이 매번 new[]/delete[](기준)
//   BufferPoolV2  2판: 스레드 캐시 + 전역 풀(묶음). 1판(등급마다 mutex) 수치는 journal 에 남겼다(커밋 2209890 으로 다시
//   잴 수 있다)

namespace
{
    constexpr std::int64_t TotalOps = std::int64_t{1} << 23; // 약 840만. 1, 2, 4, 8 과 64(묶음)로 나누어떨어진다

    // 스레드 count개를 띄워 body(스레드 번호, 맡은 횟수)를 돌리고 모두 끝날 때까지 기다린다.
    template <typename Body>
    void RunOnThreads(const int count, const Body& body)
    {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<std::size_t>(count));
        for (auto t = 0; t < count; ++t)
        {
            threads.emplace_back([&body, t, count] { body(t, TotalOps / count); });
        }
    } // jthread 소멸 = join

    // ---- 빌리기 방법 두 가지를 같은 모양으로 감싼다

    // 풀 없이 new[]/delete[]. 소멸하면 delete[].
    class NewDeleteBuffer
    {
    public:
        NewDeleteBuffer() = default;
        explicit NewDeleteBuffer(const std::size_t size) : _data(std::make_unique_for_overwrite<std::byte[]>(size))
        {
        }
        ~NewDeleteBuffer() = default;
        NewDeleteBuffer(const NewDeleteBuffer&) = delete;
        NewDeleteBuffer& operator=(const NewDeleteBuffer&) = delete;
        NewDeleteBuffer(NewDeleteBuffer&&) noexcept = default;
        NewDeleteBuffer& operator=(NewDeleteBuffer&&) noexcept = default;

        [[nodiscard]] std::byte* Data() const noexcept
        {
            return _data.get();
        }
        void Reset() noexcept
        {
            _data.reset();
        }

    private:
        std::unique_ptr<std::byte[]> _data;
    };

    struct NewDeleteRenter
    {
        using Buffer = NewDeleteBuffer;
        static Buffer Rent(const std::size_t size)
        {
            return Buffer(size);
        }
    };

    struct PoolRenter
    {
        using Buffer = Memory::PooledBuffer;
        static Buffer Rent(const std::size_t size)
        {
            return Memory::BufferPool::Shared().Rent(size);
        }
    };

    // ---- 0) 측정 틀의 한계: 아무 일도 하지 않는 반복

    void Noop(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        for (auto iteration : state)
        {
            RunOnThreads(threads,
                         [](int, const std::int64_t ops)
                         {
                             volatile std::int64_t sink{}; // 스레드마다 자기 변수(아래 주석 "DoNotOptimize")
                             for (auto i = std::int64_t{0}; i < ops; ++i)
                             {
                                 sink = i;
                             }
                         });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    // ---- 1) 같은 스레드에서 빌리고 반납

    template <typename Renter>
    void SameThread(benchmark::State& state)
    {
        const auto threads = static_cast<int>(state.range(0));
        const auto size = static_cast<std::size_t>(state.range(1));
        for (auto iteration : state)
        {
            RunOnThreads(threads,
                         [size](int, const std::int64_t ops)
                         {
                             std::byte* volatile sink{}; // 스레드마다 자기 변수(아래 주석 "DoNotOptimize")
                             for (auto i = std::int64_t{0}; i < ops; ++i)
                             {
                                 auto buffer = Renter::Rent(size);
                                 buffer.Data()[0] = std::byte{1};
                                 sink = buffer.Data(); // 주소를 밖으로 내보내 컴파일러가 할당을 지우지 못하게 한다
                             }
                         });
        }
        state.SetItemsProcessed(state.iterations() * TotalOps);
    }

    // ---- 2) 빌리는 스레드 N개 → 반납하는 스레드 M개(Bench/RentReturnGrid.h)

    template <typename Renter>
    void RentReturn(benchmark::State& state)
    {
        const auto renters = static_cast<int>(state.range(0));
        const auto returners = static_cast<int>(state.range(1));
        const auto size = static_cast<std::size_t>(state.range(2));
        Bench::RentReturnGrid<typename Renter::Buffer> grid(renters, returners); // 스레드는 측정 밖에서 한 번 만든다
        auto ops = std::int64_t{};                                               // 실제로 한 횟수(조합의 배수로 내림)
        for (auto iteration : state)
        {
            ops = grid.Run(Bench::RentReturnOps,
                           [size]
                           {
                               auto buffer = Renter::Rent(size);
                               buffer.Data()[0] = std::byte{1};
                               return buffer;
                           });
        }
        state.SetItemsProcessed(state.iterations() * ops);
    }
} // namespace

// 인자: {스레드 수, 크기}. 크기: 256B(이동 패킷), 4KB(큰 패킷).
BENCHMARK(Noop)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<NewDeleteRenter>)
    ->Name("SameThread/NewDelete")
    ->ArgsProduct({{1, 2, 4, 8}, {256, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(SameThread<PoolRenter>)
    ->Name("SameThread/BufferPoolV2")
    ->ArgsProduct({{1, 2, 4, 8}, {256, 4096}})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
// 인자: {빌리는 스레드 N, 반납하는 스레드 M, 크기}. 조합은 Bench::ApplyRentReturnShapesWithSizes(코어 수 기준).
// 측정 틀 자체의 비용은 ObjectPoolBench 의 RentReturnNoop(같은 틀)으로 본다.
#define RENT_RETURN_BENCH(renter, name)                                                                                \
    BENCHMARK(RentReturn<renter>)                                                                                      \
        ->Name(name)                                                                                                   \
        ->Apply(Bench::ApplyRentReturnShapesWithSizes)                                                                 \
        ->UseRealTime()                                                                                                \
        ->Unit(benchmark::kMillisecond)
RENT_RETURN_BENCH(NewDeleteRenter, "BufferRentReturn/NewDelete");
RENT_RETURN_BENCH(PoolRenter, "BufferRentReturn/BufferPoolV2");
