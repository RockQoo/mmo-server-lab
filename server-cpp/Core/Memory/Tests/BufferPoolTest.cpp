#include <gtest/gtest.h>

#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

#include "Memory/BufferPool.h"

using Memory::BufferPool;
using Memory::PooledBuffer;

TEST(BufferPool, RentRoundsUpToSizeClass)
{
    BufferPool pool;
    EXPECT_EQ(pool.Rent(0).Size(), 256u);
    EXPECT_EQ(pool.Rent(1).Size(), 256u);
    EXPECT_EQ(pool.Rent(256).Size(), 256u);
    EXPECT_EQ(pool.Rent(257).Size(), 1024u);
    EXPECT_EQ(pool.Rent(4096).Size(), 4096u);
    EXPECT_EQ(pool.Rent(BufferPool::MaxSize).Size(), BufferPool::MaxSize);
}

TEST(BufferPool, ReturnedBufferIsReused)
{
    BufferPool pool;
    auto first = pool.Rent(100);
    const auto* data = first.Data();
    first.Reset();
    EXPECT_TRUE(first.IsEmpty());
    EXPECT_EQ(pool.CachedCount(), 1u);

    const auto second = pool.Rent(200); // 같은 등급(256)이라 방금 반납한 버퍼
    EXPECT_EQ(second.Data(), data);
    EXPECT_EQ(pool.CachedCount(), 0u);
}

// 등급이 다르면 섞이지 않는다.
TEST(BufferPool, SizeClassesAreSeparate)
{
    BufferPool pool;
    pool.Rent(100).Reset();
    const auto large = pool.Rent(1000);
    EXPECT_EQ(large.Size(), 1024u);
    EXPECT_EQ(pool.CachedCount(), 1u) << "256 등급 버퍼는 그대로 남아 있다";
}

TEST(BufferPool, DestructorReturns)
{
    BufferPool pool;
    {
        const auto buffer = pool.Rent(100);
        EXPECT_EQ(pool.RentedCount(), 1u);
    }
    EXPECT_EQ(pool.RentedCount(), 0u);
    EXPECT_EQ(pool.CachedCount(), 1u);
}

TEST(BufferPool, MoveTransfersOwnership)
{
    BufferPool pool;
    auto source = pool.Rent(100);
    const auto* data = source.Data();

    PooledBuffer moved(std::move(source));
    EXPECT_TRUE(source.IsEmpty()); // NOLINT(bugprone-use-after-move): 옮긴 뒤 빈 핸들인지 확인
    EXPECT_EQ(moved.Data(), data);
    EXPECT_EQ(pool.RentedCount(), 1u) << "옮겨도 반납되지 않는다";
}

TEST(BufferPool, MoveAssignReturnsHeldBuffer)
{
    BufferPool pool;
    auto target = pool.Rent(100);
    auto source = pool.Rent(100);
    target = std::move(source);
    EXPECT_EQ(pool.RentedCount(), 1u);
    EXPECT_EQ(pool.CachedCount(), 1u) << "target이 들고 있던 버퍼는 반납된다";
}

// 64KB보다 크면 풀에 두지 않는다.
TEST(BufferPool, LargerThanMaxIsNotPooled)
{
    BufferPool pool;
    const auto size = BufferPool::MaxSize + 1;
    {
        const auto large = pool.Rent(size);
        EXPECT_EQ(large.Size(), size);
        EXPECT_EQ(pool.RentedCount(), 1u);
    }
    EXPECT_EQ(pool.RentedCount(), 0u);
    EXPECT_EQ(pool.CachedCount(), 0u);
}

// 여러 스레드가 동시에 빌리고 반납해도 같은 버퍼를 두 곳이 쥐지 않는다.
// 빌린 버퍼에 자기 번호를 채우고 잠깐 양보한 뒤 그대로인지 확인한다(다른 스레드가 같은 버퍼를 받았다면 덮어써진다).
TEST(BufferPool, ConcurrentRentNeverShares)
{
    constexpr auto ThreadCount = 8;
    constexpr auto Rounds = 5000;
    BufferPool pool;
    {
        std::vector<std::jthread> threads;
        for (auto t = 0; t < ThreadCount; ++t)
        {
            threads.emplace_back(
                [&pool, t]
                {
                    const auto mark = static_cast<std::byte>(t + 1);
                    for (auto i = 0; i < Rounds; ++i)
                    {
                        const auto buffer = pool.Rent(static_cast<std::size_t>(64 + (i % 3) * 600));
                        for (auto& b : buffer.Span())
                        {
                            b = mark;
                        }
                        std::this_thread::yield();
                        for (const auto b : buffer.Span())
                        {
                            ASSERT_EQ(b, mark);
                        }
                    }
                });
        }
    }
    EXPECT_EQ(pool.RentedCount(), 0u);
    EXPECT_LE(pool.CachedCount(), static_cast<std::size_t>(ThreadCount * 3))
        << "스레드 수 × 등급 수 이상 쌓이지 않는다";
}

TEST(BufferPoolDeathTest, DestroyWhileRentedCrashes)
{
    EXPECT_DEATH(
        {
            PooledBuffer buffer;
            {
                BufferPool pool;
                buffer = pool.Rent(100);
            }
        },
        "still rented");
}
