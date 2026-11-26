#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Memory/ObjectPool.h"

using Memory::ObjectPool;
using Memory::Poolable;
using Memory::PooledPtr;

// 풀은 타입마다 하나다. 테스트마다 다른 타입(Item<번호>)을 써서 다른 테스트가 남긴 객체가 섞이지 않게 한다.
// 그래도 같은 프로세스에서 반복 실행(--gtest_repeat)될 수 있어 개수는 "전후 차이"로 확인한다.

namespace
{
    // 시험용 객체: 값 하나와 vector 하나. Reset()은 값을 비우고 vector는 clear()(용량은 남김).
    template <int Tag>
    struct Item
    {
        std::uint64_t id{};
        std::vector<int> values;

        void Reset() noexcept
        {
            id = 0;
            values.clear();
        }
    };

    template <int Tag>
    ObjectPool<Item<Tag>>& Pool()
    {
        return ObjectPool<Item<Tag>>::Shared();
    }

    template <typename Function>
    void RunOnNewThread(Function&& function)
    {
        std::jthread thread(std::forward<Function>(function));
        thread.join();
    }

    // Poolable 이 아닌 타입들
    struct NoReset
    {
    };
    struct ThrowingReset
    {
        void Reset()
        {
        }
    };
    struct NoDefault
    {
        explicit NoDefault(int)
        {
        }
        void Reset() noexcept
        {
        }
    };

    struct alignas(64) Aligned
    {
        std::byte data[64]{};
        void Reset() noexcept
        {
        }
    };

    // 생성자가 던질 수 있는 타입
    struct Throwing
    {
        static inline bool s_throwNext = false;
        Throwing()
        {
            if (s_throwNext)
            {
                s_throwNext = false;
                throw std::runtime_error("생성 실패");
            }
        }
        void Reset() noexcept
        {
        }
    };

    struct SmallCache
    {
        void Reset() noexcept
        {
        }
    };
} // namespace

template <>
struct Memory::ObjectPoolTraits<SmallCache>
{
    static constexpr std::size_t CacheCount{4};
};

TEST(ObjectPool, OnlyPoolableTypesCompile)
{
    static_assert(Poolable<Item<0>>);
    static_assert(!Poolable<NoReset>, "Reset()이 없으면 안 된다");
    static_assert(!Poolable<ThrowingReset>, "Reset()이 noexcept 가 아니면 안 된다");
    static_assert(!Poolable<NoDefault>, "기본 생성이 안 되면 안 된다");
    SUCCEED();
}

TEST(ObjectPool, HandleIsPointerSized)
{
    static_assert(sizeof(PooledPtr<Item<0>>) == sizeof(Item<0>*), "Deleter 에 상태가 없어 포인터 하나 크기");
    SUCCEED();
}

TEST(ObjectPool, RentGivesDefaultObject)
{
    const auto item = Pool<1>().Rent();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->id, 0u);
    EXPECT_TRUE(item->values.empty());
}

// 반납하면 Reset()되고, 같은 스레드의 다음 Rent가 같은 객체를 준다. vector 용량은 남아 있다(1B 방식의 이유).
TEST(ObjectPool, ReturnResetsAndKeepsCapacity)
{
    auto first = Pool<2>().Rent();
    first->id = 42;
    first->values.assign(100, 7);
    const auto* address = first.get();
    const auto capacity = first->values.capacity();
    const auto before = ObjectPool<Item<2>>::ThreadCachedCount();
    first.reset();
    EXPECT_EQ(ObjectPool<Item<2>>::ThreadCachedCount(), before + 1);

    const auto second = Pool<2>().Rent();
    EXPECT_EQ(second.get(), address) << "같은 스레드 캐시에서 같은 객체";
    EXPECT_EQ(second->id, 0u);
    EXPECT_TRUE(second->values.empty());
    EXPECT_EQ(second->values.capacity(), capacity) << "용량은 남아 다시 할당하지 않는다";
}

TEST(ObjectPool, MoveTransfersOwnership)
{
    auto source = Pool<3>().Rent();
    const auto* address = source.get();
    const auto before = ObjectPool<Item<3>>::ThreadCachedCount();
    const PooledPtr<Item<3>> moved(std::move(source));
    EXPECT_EQ(source, nullptr); // NOLINT(bugprone-use-after-move): 옮긴 뒤 빈 핸들인지 확인
    EXPECT_EQ(moved.get(), address);
    EXPECT_EQ(ObjectPool<Item<3>>::ThreadCachedCount(), before) << "옮겨도 반납되지 않는다";
}

// 캐시가 꽉 찬 채로 반납하면 묶음 하나(BatchCount)를 전역에 보낸다.
TEST(ObjectPool, OverflowSendsBatchToGlobal)
{
    using Type = Item<4>;
    RunOnNewThread(
        []
        {
            std::vector<PooledPtr<Type>> held;
            for (auto i = std::size_t{0}; i < ObjectPool<Type>::CacheCount + 1; ++i)
            {
                held.push_back(Pool<4>().Rent());
            }
            auto cached = ObjectPool<Type>::ThreadCachedCount();
            auto global = Pool<4>().GlobalCachedCount();
            auto overflowed = false;
            while (!held.empty())
            {
                if (cached == ObjectPool<Type>::CacheCount)
                {
                    cached -= ObjectPool<Type>::BatchCount;
                    global += ObjectPool<Type>::BatchCount;
                    overflowed = true;
                }
                ++cached;
                held.pop_back();
                ASSERT_EQ(ObjectPool<Type>::ThreadCachedCount(), cached);
                ASSERT_EQ(Pool<4>().GlobalCachedCount(), global);
            }
            EXPECT_TRUE(overflowed) << "캐시보다 하나 많이 반납하면 적어도 한 번은 넘친다";
        });
}

// 스레드가 끝나면 캐시에 남은 객체가 모두 전역으로 간다.
TEST(ObjectPool, ThreadExitFlushesCacheToGlobal)
{
    auto globalAtExit = std::size_t{};
    auto cachedAtExit = std::size_t{};
    RunOnNewThread(
        [&globalAtExit, &cachedAtExit]
        {
            {
                auto a = Pool<5>().Rent();
                auto b = Pool<5>().Rent();
                auto c = Pool<5>().Rent();
            }
            globalAtExit = Pool<5>().GlobalCachedCount();
            cachedAtExit = ObjectPool<Item<5>>::ThreadCachedCount();
        });
    EXPECT_GE(cachedAtExit, 3u);
    EXPECT_EQ(Pool<5>().GlobalCachedCount(), globalAtExit + cachedAtExit);
}

// 캐시가 빈 스레드는 전역에서 한 묶음을 가져온다. 가져온 객체는 Reset()된 상태다.
TEST(ObjectPool, EmptyCacheTakesBatchFromGlobal)
{
    RunOnNewThread(
        []
        {
            auto item = Pool<6>().Rent();
            item->id = 99;
            item->values.push_back(1);
        }); // 반납(Reset) → 캐시 → 스레드 끝에 전역으로
    RunOnNewThread(
        []
        {
            const auto globalBefore = Pool<6>().GlobalCachedCount();
            ASSERT_GT(globalBefore, 0u);
            const auto item = Pool<6>().Rent();
            const auto taken = globalBefore - Pool<6>().GlobalCachedCount();
            EXPECT_GE(taken, 1u);
            EXPECT_EQ(ObjectPool<Item<6>>::ThreadCachedCount(), taken - 1) << "하나는 빌려주고 나머지는 캐시에";
            EXPECT_EQ(item->id, 0u);
            EXPECT_TRUE(item->values.empty());
        });
}

// 다른 스레드에서 반납하면 반납한 스레드의 캐시로 간다(Lane이 빌리고 IOCP 스레드가 반납하는 모양).
TEST(ObjectPool, ReturnOnOtherThreadGoesToThatThreadsCache)
{
    auto item = Pool<7>().Rent();
    const auto mineBefore = ObjectPool<Item<7>>::ThreadCachedCount();
    RunOnNewThread(
        [moved = std::move(item)]() mutable
        {
            const auto before = ObjectPool<Item<7>>::ThreadCachedCount();
            moved.reset();
            EXPECT_EQ(ObjectPool<Item<7>>::ThreadCachedCount(), before + 1);
        });
    EXPECT_EQ(ObjectPool<Item<7>>::ThreadCachedCount(), mineBefore) << "빌린 스레드의 캐시는 그대로";
}

// 여러 스레드가 동시에 빌리고 반납해도 같은 객체를 두 곳이 쥐지 않는다(자기 번호를 적고 양보한 뒤 그대로인지 확인).
TEST(ObjectPool, ConcurrentRentNeverShares)
{
    constexpr auto ThreadCount = 8;
    constexpr auto Rounds = 5000;
    std::vector<std::jthread> threads;
    for (auto t = 0; t < ThreadCount; ++t)
    {
        threads.emplace_back(
            [t]
            {
                const auto mark = static_cast<std::uint64_t>(t) + 1;
                std::vector<PooledPtr<Item<8>>> held; // 가끔 여러 개를 들고 있어 캐시가 넘치고 비게 한다
                for (auto i = 0; i < Rounds; ++i)
                {
                    auto item = Pool<8>().Rent();
                    ASSERT_EQ(item->id, 0u) << "Reset()된 객체만 받는다";
                    item->id = mark;
                    item->values.push_back(t);
                    std::this_thread::yield();
                    ASSERT_EQ(item->id, mark);
                    ASSERT_EQ(item->values.size(), 1u);
                    held.push_back(std::move(item));
                    if (held.size() == 100)
                    {
                        held.clear();
                    }
                }
            });
    }
}

// 빌리는 스레드와 반납하는 스레드가 달라도 같은 객체를 두 곳이 쥐지 않는다.
TEST(ObjectPool, CrossThreadRentReturnNeverShares)
{
    using Type = Item<9>;
    constexpr auto PairCount = 4;
    constexpr auto PerPair = 20'000;
    struct Channel
    {
        Channel() = default;
        ~Channel() = default;
        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;
        Channel(Channel&&) = delete;
        Channel& operator=(Channel&&) = delete;

        std::mutex mutex;
        std::queue<std::pair<std::uint64_t, PooledPtr<Type>>> items;
    };
    std::array<Channel, PairCount> channels{};
    {
        std::vector<std::jthread> threads;
        for (auto p = 0; p < PairCount; ++p)
        {
            auto& channel = channels[static_cast<std::size_t>(p)];
            threads.emplace_back(
                [&channel, p]
                {
                    for (auto i = 0; i < PerPair; ++i)
                    {
                        const auto id = ((static_cast<std::uint64_t>(p) + 1) << 32) | static_cast<std::uint64_t>(i);
                        auto item = Pool<9>().Rent();
                        item->id = id;
                        const std::scoped_lock lock(channel.mutex);
                        channel.items.emplace(id, std::move(item));
                    }
                });
            threads.emplace_back(
                [&channel]
                {
                    auto received = 0;
                    while (received < PerPair)
                    {
                        std::pair<std::uint64_t, PooledPtr<Type>> entry;
                        {
                            const std::scoped_lock lock(channel.mutex);
                            if (channel.items.empty())
                            {
                                continue;
                            }
                            entry = std::move(channel.items.front());
                            channel.items.pop();
                        }
                        ASSERT_EQ(entry.second->id, entry.first);
                        entry.second.reset(); // 이 스레드 캐시로 반납
                        ++received;
                    }
                });
        }
    }
#ifndef NDEBUG
    EXPECT_EQ(Pool<9>().RentedCount(), 0) << "모두 반납됐다(Debug에서만 센다)";
#endif
}

// 정렬이 큰 타입도 그 정렬로 받는다.
TEST(ObjectPool, KeepsAlignment)
{
    std::vector<PooledPtr<Aligned>> held;
    for (auto i = 0; i < 10; ++i)
    {
        held.push_back(ObjectPool<Aligned>::Shared().Rent());
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(held.back().get()) % 64, 0u);
    }
}

// 생성자가 던지면 Rent도 던지고, 풀은 그 뒤에도 정상이다.
TEST(ObjectPool, ConstructorThrowPropagates)
{
    RunOnNewThread(
        []
        {
            Throwing::s_throwNext = true;
            EXPECT_THROW((void)ObjectPool<Throwing>::Shared().Rent(), std::runtime_error);
            const auto item = ObjectPool<Throwing>::Shared().Rent();
            EXPECT_NE(item, nullptr);
#ifndef NDEBUG
            EXPECT_EQ(ObjectPool<Throwing>::Shared().RentedCount(), 1) << "실패한 Rent는 세지 않는다";
#endif
        });
}

// 캐시 개수는 타입마다 바꿀 수 있다(ObjectPoolTraits 특수화).
TEST(ObjectPool, TraitsChangeCacheCount)
{
    static_assert(ObjectPool<Item<0>>::CacheCount == 64 && ObjectPool<Item<0>>::BatchCount == 32);
    static_assert(ObjectPool<SmallCache>::CacheCount == 4 && ObjectPool<SmallCache>::BatchCount == 2);
    RunOnNewThread(
        []
        {
            std::vector<PooledPtr<SmallCache>> held;
            for (auto i = 0; i < 5; ++i)
            {
                held.push_back(ObjectPool<SmallCache>::Shared().Rent());
            }
            auto cached = ObjectPool<SmallCache>::ThreadCachedCount();
            auto global = ObjectPool<SmallCache>::Shared().GlobalCachedCount();
            while (!held.empty()) // 캐시 4개가 꽉 찬 채로 반납하면 2개가 전역으로
            {
                if (cached == 4)
                {
                    cached -= 2;
                    global += 2;
                }
                ++cached;
                held.pop_back();
            }
            EXPECT_EQ(ObjectPool<SmallCache>::ThreadCachedCount(), cached);
            EXPECT_EQ(ObjectPool<SmallCache>::Shared().GlobalCachedCount(), global);
        });
}

#ifndef NDEBUG
TEST(ObjectPool, DebugCountsRented)
{
    const auto before = Pool<10>().RentedCount();
    {
        const auto a = Pool<10>().Rent();
        const auto b = Pool<10>().Rent();
        EXPECT_EQ(Pool<10>().RentedCount(), before + 2);
    }
    EXPECT_EQ(Pool<10>().RentedCount(), before);
}
#endif
