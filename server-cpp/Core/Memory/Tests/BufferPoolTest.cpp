#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "Memory/BufferPool.h"

using Memory::BufferPool;
using Memory::PooledBuffer;

// 풀은 프로세스에 하나라 다른 테스트가 남긴 버퍼가 있을 수 있다. 개수는 "전후 차이"로 확인한다.
// 빈 스레드 캐시에서 시작해야 하는 테스트는 새 스레드에서 돈다.

namespace
{
    BufferPool& Pool()
    {
        return BufferPool::Shared();
    }

    constexpr auto Small = std::size_t{100}; // 256B 등급

    // 새 스레드에서 돌리고 끝날 때까지 기다린다(스레드 캐시가 빈 상태에서 시작, 끝나면 캐시가 전역으로).
    template <typename Function>
    void RunOnNewThread(Function&& function)
    {
        std::jthread thread(std::forward<Function>(function));
        thread.join();
    }
} // namespace

TEST(BufferPool, RentRoundsUpToSizeClass)
{
    EXPECT_EQ(Pool().Rent(0).Size(), 256u);
    EXPECT_EQ(Pool().Rent(1).Size(), 256u);
    EXPECT_EQ(Pool().Rent(256).Size(), 256u);
    EXPECT_EQ(Pool().Rent(257).Size(), 1024u);
    EXPECT_EQ(Pool().Rent(4096).Size(), 4096u);
    EXPECT_EQ(Pool().Rent(BufferPool::MaxSize).Size(), BufferPool::MaxSize);
}

// 같은 스레드에서 반납한 버퍼는 그 스레드 캐시로 가고, 다음 Rent가 그대로 다시 준다(락 없는 길).
TEST(BufferPool, SameThreadReuseComesFromThreadCache)
{
    auto first = Pool().Rent(Small);
    const auto* data = first.Data();
    const auto before = BufferPool::ThreadCachedCount(Small);
    first.Reset();
    EXPECT_TRUE(first.IsEmpty());
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), before + 1);

    const auto second = Pool().Rent(200); // 같은 등급(256)
    EXPECT_EQ(second.Data(), data);
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), before);
}

// 등급이 다르면 섞이지 않는다.
TEST(BufferPool, SizeClassesAreSeparate)
{
    Pool().Rent(Small).Reset();
    const auto smallCached = BufferPool::ThreadCachedCount(Small);
    const auto large = Pool().Rent(1000);
    EXPECT_EQ(large.Size(), 1024u);
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), smallCached) << "256 등급 버퍼는 그대로 남아 있다";
}

TEST(BufferPool, MoveTransfersOwnership)
{
    auto source = Pool().Rent(Small);
    const auto* data = source.Data();
    const auto before = BufferPool::ThreadCachedCount(Small);

    PooledBuffer moved(std::move(source));
    EXPECT_TRUE(source.IsEmpty()); // NOLINT(bugprone-use-after-move): 옮긴 뒤 빈 핸들인지 확인
    EXPECT_EQ(moved.Data(), data);
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), before) << "옮겨도 반납되지 않는다";
}

TEST(BufferPool, MoveAssignReturnsHeldBuffer)
{
    auto target = Pool().Rent(Small);
    auto source = Pool().Rent(Small);
    const auto before = BufferPool::ThreadCachedCount(Small);
    target = std::move(source);
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), before + 1) << "target이 들고 있던 버퍼는 반납된다";
}

// 64KB보다 크면 풀에 두지 않는다.
TEST(BufferPool, LargerThanMaxIsNotPooled)
{
    const auto size = BufferPool::MaxSize + 1;
    {
        const auto large = Pool().Rent(size);
        EXPECT_EQ(large.Size(), size);
    }
    EXPECT_EQ(BufferPool::ThreadCachedCount(size), 0u);
    EXPECT_EQ(Pool().GlobalCachedCount(size), 0u);
}

// 캐시가 꽉 찬 채로 반납하면 묶음 하나(BatchCountFor)를 전역에 보낸다.
// 앞 테스트가 전역에 남긴 묶음을 Rent가 가져올 수 있어서, 반납 직전 개수에서 규칙대로 계산한 값과 비교한다.
TEST(BufferPool, OverflowSendsBatchToGlobal)
{
    RunOnNewThread(
        []
        {
            std::vector<PooledBuffer> held;
            const auto capacity = BufferPool::CacheCountFor(BufferPool::SizeFor(Small));
            const auto batch = BufferPool::BatchCountFor(BufferPool::SizeFor(Small));
            for (auto i = std::size_t{0}; i < capacity + 1; ++i)
            {
                held.push_back(Pool().Rent(Small));
            }
            auto cached = BufferPool::ThreadCachedCount(Small);
            auto global = Pool().GlobalCachedCount(Small);
            auto overflowed = false;
            while (!held.empty())
            {
                if (cached == capacity) // 꽉 찬 채로 반납 → 한 묶음이 전역으로
                {
                    cached -= batch;
                    global += batch;
                    overflowed = true;
                }
                ++cached;
                held.pop_back();
                ASSERT_EQ(BufferPool::ThreadCachedCount(Small), cached);
                ASSERT_EQ(Pool().GlobalCachedCount(Small), global);
            }
            EXPECT_TRUE(overflowed) << "캐시보다 하나 많이 반납하면 적어도 한 번은 넘친다";
        });
}

// 스레드가 끝나면 캐시에 남은 버퍼가 모두 전역으로 간다.
TEST(BufferPool, ThreadExitFlushesCacheToGlobal)
{
    auto globalAtExit = std::size_t{};
    auto cachedAtExit = std::size_t{};
    RunOnNewThread(
        [&globalAtExit, &cachedAtExit]
        {
            {
                auto a = Pool().Rent(Small);
                auto b = Pool().Rent(Small);
                auto c = Pool().Rent(Small);
            } // a, b, c 반납 → 이 스레드 캐시로
            globalAtExit = Pool().GlobalCachedCount(Small);
            cachedAtExit = BufferPool::ThreadCachedCount(Small);
        });
    EXPECT_GE(cachedAtExit, 3u);
    EXPECT_EQ(Pool().GlobalCachedCount(Small), globalAtExit + cachedAtExit) << "스레드가 끝나며 캐시를 전역으로 보냄";
}

// 스레드가 끝날 때 thread_local 핸들과 스레드 캐시가 어느 순서로 소멸해도 버퍼 수가 맞는다(Codex 리뷰 R1).
//   핸들이 먼저 소멸: 반납 → 캐시 → 캐시 소멸 때 전역으로
//   캐시가 먼저 소멸: 반납 → 캐시가 없으므로 전역으로 바로(R1 수정. 고치기 전에는 소멸한 캐시에 썼다)
// 표준은 두 번째 순서를 허용한다(메인 스레드의 thread_local 은 정적 객체보다 먼저 소멸). 다만 MSVC는 파일 범위
// thread_local(스레드 캐시)을 스레드 시작 때 미리 만들어, 이 테스트에서는 첫 번째 순서만 일어난다(수정 전 코드로도
// 통과함을 확인했다). 그래서 이 테스트는 R1을 재현하지 못하고, 두 순서 모두에서 맞아야 할 결과만 확인한다.
TEST(BufferPool, ThreadLocalHolderReturnKeepsCount)
{
    auto globalAtExit = std::size_t{};
    auto cachedAtExit = std::size_t{};
    RunOnNewThread(
        [&globalAtExit, &cachedAtExit]
        {
            thread_local PooledBuffer t_held; // 스레드가 끝날 때 소멸
            t_held = Pool().Rent(Small);
            globalAtExit = Pool().GlobalCachedCount(Small);
            cachedAtExit = BufferPool::ThreadCachedCount(Small);
        }); // 스레드 끝: 어느 순서든 캐시의 버퍼와 t_held 의 버퍼가 모두 전역으로 간다
    EXPECT_EQ(Pool().GlobalCachedCount(Small), globalAtExit + cachedAtExit + 1);
#ifndef NDEBUG
    EXPECT_EQ(Pool().RentedCount(), 0);
#endif
}

// 캐시가 빈 스레드는 전역에서 한 묶음을 가져온다.
TEST(BufferPool, EmptyCacheTakesBatchFromGlobal)
{
    RunOnNewThread([] { auto a = Pool().Rent(Small); }); // 전역에 적어도 한 묶음을 만든다(스레드 끝)
    RunOnNewThread(
        []
        {
            const auto globalBefore = Pool().GlobalCachedCount(Small);
            ASSERT_GT(globalBefore, 0u);
            const auto buffer = Pool().Rent(Small);
            const auto taken = globalBefore - Pool().GlobalCachedCount(Small);
            EXPECT_GE(taken, 1u) << "묶음 하나를 가져온다";
            EXPECT_EQ(BufferPool::ThreadCachedCount(Small), taken - 1) << "하나는 빌려주고 나머지는 캐시에";
        });
}

// 다른 스레드에서 반납하면 반납한 스레드의 캐시로 간다(송신 완료를 IOCP 스레드가 반납하는 모양).
TEST(BufferPool, ReturnOnOtherThreadGoesToThatThreadsCache)
{
    auto buffer = Pool().Rent(Small);
    const auto mineBefore = BufferPool::ThreadCachedCount(Small);
    RunOnNewThread(
        [moved = std::move(buffer)]() mutable
        {
            const auto before = BufferPool::ThreadCachedCount(Small);
            moved.Reset();
            EXPECT_EQ(BufferPool::ThreadCachedCount(Small), before + 1);
        });
    EXPECT_EQ(BufferPool::ThreadCachedCount(Small), mineBefore) << "빌린 스레드의 캐시는 그대로";
}

// 여러 스레드가 동시에 빌리고 반납해도 같은 버퍼를 두 곳이 쥐지 않는다.
// 빌린 버퍼에 자기 번호를 채우고 잠깐 양보한 뒤 그대로인지 확인한다(다른 스레드가 같은 버퍼를 받았다면 덮어써진다).
TEST(BufferPool, ConcurrentRentNeverShares)
{
    constexpr auto ThreadCount = int32_t{8};
    constexpr auto Rounds = int32_t{5000};
    std::vector<std::jthread> threads;
    for (auto t = int32_t{0}; t < ThreadCount; ++t)
    {
        threads.emplace_back(
            [t]
            {
                const auto mark = static_cast<std::byte>(t + 1);
                std::vector<PooledBuffer> held; // 가끔 여러 개를 들고 있어 캐시가 넘치고 비게 한다
                for (auto i = int32_t{0}; i < Rounds; ++i)
                {
                    auto buffer = Pool().Rent(static_cast<std::size_t>(64 + (i % 3) * 600));
                    for (auto& b : buffer.Span())
                    {
                        b = mark;
                    }
                    std::this_thread::yield();
                    for (const auto b : buffer.Span())
                    {
                        ASSERT_EQ(b, mark);
                    }
                    held.push_back(std::move(buffer));
                    if (held.size() == 40)
                    {
                        held.clear();
                    }
                }
            });
    }
}

// 빌리는 스레드와 반납하는 스레드가 달라도 같은 버퍼를 두 곳이 쥐지 않는다.
// 빌리는 쪽이 버퍼에 고유 번호를 적어 넘기고, 반납하는 쪽이 번호가 그대로인지 확인한 뒤 반납한다.
TEST(BufferPool, CrossThreadRentReturnNeverShares)
{
    constexpr auto PairCount = int32_t{4};
    constexpr auto PerPair = int32_t{20'000};
    struct Channel
    {
        Channel() = default;
        ~Channel() = default;
        Channel(const Channel&) = delete; // mutex를 들고 있어 옮길 수 없다
        Channel& operator=(const Channel&) = delete;
        Channel(Channel&&) = delete;
        Channel& operator=(Channel&&) = delete;

        std::mutex mutex;
        std::queue<std::pair<uint64_t, PooledBuffer>> items;
    };
    std::array<Channel, PairCount> channels{};
    {
        std::vector<std::jthread> threads;
        for (auto p = int32_t{0}; p < PairCount; ++p)
        {
            auto& channel = channels[static_cast<std::size_t>(p)];
            threads.emplace_back(
                [&channel, p]
                {
                    for (auto i = int32_t{0}; i < PerPair; ++i)
                    {
                        const auto id = (static_cast<uint64_t>(p) << 32) | static_cast<uint64_t>(i);
                        auto buffer = Pool().Rent(Small);
                        std::memcpy(buffer.Data(), &id, sizeof(id));
                        const std::scoped_lock lock(channel.mutex);
                        channel.items.emplace(id, std::move(buffer));
                    }
                });
            threads.emplace_back(
                [&channel]
                {
                    auto received = int32_t{0};
                    while (received < PerPair)
                    {
                        std::pair<uint64_t, PooledBuffer> item;
                        {
                            const std::scoped_lock lock(channel.mutex);
                            if (channel.items.empty())
                            {
                                continue;
                            }
                            item = std::move(channel.items.front());
                            channel.items.pop();
                        }
                        auto written = uint64_t{};
                        std::memcpy(&written, item.second.Data(), sizeof(written));
                        ASSERT_EQ(written, item.first);
                        item.second.Reset(); // 이 스레드 캐시로 반납
                        ++received;
                    }
                });
        }
    }
#ifndef NDEBUG
    EXPECT_EQ(Pool().RentedCount(), 0) << "모두 반납됐다(Debug에서만 센다)";
#endif
}

#ifndef NDEBUG
// Debug에서만 빌려 간 수를 센다.
TEST(BufferPool, DebugCountsRented)
{
    const auto before = Pool().RentedCount();
    {
        const auto a = Pool().Rent(Small);
        const auto b = Pool().Rent(BufferPool::MaxSize + 1); // 풀 밖 큰 버퍼도 센다
        EXPECT_EQ(Pool().RentedCount(), before + 2);
    }
    EXPECT_EQ(Pool().RentedCount(), before);
}
#endif

// 캐시 개수는 등급마다 다르다: 들고 있는 바이트를 CacheBytes(256KB)에 맞추고 4~64개로 자른다.
TEST(BufferPool, CacheCountFollowsByteBudget)
{
    static_assert(BufferPool::CacheCountFor(256) == 64);
    static_assert(BufferPool::CacheCountFor(1024) == 64);
    static_assert(BufferPool::CacheCountFor(4096) == 64);
    static_assert(BufferPool::CacheCountFor(std::size_t{16} * 1024) == 16);
    static_assert(BufferPool::CacheCountFor(std::size_t{64} * 1024) == 4);
    static_assert(BufferPool::BatchCountFor(256) == 32);
    static_assert(BufferPool::BatchCountFor(std::size_t{64} * 1024) == 2);
    SUCCEED();
}
