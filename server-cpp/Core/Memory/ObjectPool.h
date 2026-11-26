#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>

namespace Memory
{
    // ObjectPool 에 넣을 수 있는 타입: 기본 생성이 되고, 예외 없는 Reset()이 있어야 한다(없으면 컴파일 에러).
    // Reset()은 반납할 때 불린다. 다음 사용자가 이전 값을 보지 않게 모든 필드를 비우되, 내부 컨테이너의 용량은 남긴다
    // (vector는 clear(), 새 vector를 대입하면 용량이 사라져 풀을 쓰는 의미가 없다). memset 으로 밀면 안 된다.
    template <typename T>
    concept Poolable = std::default_initializable<T> && requires(T& object) {
        { object.Reset() } noexcept;
    };

    // 타입마다 스레드 캐시 개수를 바꾸고 싶으면 특수화한다(기본 64개, 묶음은 그 절반).
    //   template <> struct Memory::ObjectPoolTraits<BigObject> { static constexpr std::size_t CacheCount{8}; };
    template <typename T>
    struct ObjectPoolTraits
    {
        static constexpr std::size_t CacheCount{64};
    };

    // 객체를 다시 쓰는 풀. 타입마다 프로세스에 하나(Shared)이고 프로그램이 끝날 때까지 산다.
    // 반납된 객체는 소멸시키지 않고 Reset()만 해서 들고 있다가 다음 Rent에 준다(내부 vector 등의 용량까지 재사용).
    //
    // 구조는 BufferPool 2판과 같다(그림은 docs/journal/objectpool.html)
    //   스레드 캐시  스레드마다 반납된 객체를 CacheCount개까지 들고 있다. 보통의 Rent·반납은 여기서 끝난다(락 없음).
    //   전역 풀      mutex 하나 + 묶음 목록. 캐시가 넘치면 BatchCount개를 한 묶음으로 보내고, 비면 한 묶음을 가져온다.
    //   스레드 종료  캐시에 남은 객체를 전역으로 보낸다.
    // 다른 점: BufferPool은 빈 버퍼 안에 연결 정보를 덮어쓰지만, 여기의 객체는 살아 있어 덮어쓸 수 없다. 그래서
    // 객체마다 바로 뒤에 연결 칸(Links)을 붙여 한 번에 할당한다([T][Links]).
    //
    // 빌린 스레드와 반납하는 스레드가 달라도 된다. Reset()은 반납하는 스레드에서 불린다.
    // 객체는 OS에 돌려주지 않는다(상한 없음, 소멸자도 부르지 않는다. 프로그램이 끝나면 OS가 메모리를 회수한다).
    // C#은 이 클래스를 만들지 않는다(Microsoft.Extensions.ObjectPool, 이유는 docs/naming.md).
    template <Poolable T>
    class ObjectPool
    {
    public:
        // 핸들이 소멸하면 풀로 반납한다. 상태가 없어 핸들(unique_ptr)은 포인터 하나 크기다.
        struct Deleter
        {
            void operator()(T* object) const noexcept
            {
                ObjectPool::Shared().Return(object);
            }
        };

        static constexpr std::size_t CacheCount{ObjectPoolTraits<T>::CacheCount};
        static constexpr std::size_t BatchCount{CacheCount / 2};
        static_assert(BatchCount > 0, "CacheCount는 2 이상이어야 한다");

        // 이 타입의 하나뿐인 풀.
        [[nodiscard]] static ObjectPool& Shared() noexcept
        {
            return s_shared;
        }

        ObjectPool(const ObjectPool&) = delete;
        ObjectPool& operator=(const ObjectPool&) = delete;
        ObjectPool(ObjectPool&&) = delete;
        ObjectPool& operator=(ObjectPool&&) = delete;

        // 객체 하나를 빌린다. 내 캐시 → 전역 묶음 → 새로 생성 순서로 찾는다. 받은 객체는 새것이거나 Reset()된 것이다.
        // 생성자 인자는 받지 않는다(다시 쓰는 객체는 이미 생성돼 있다). 값은 빌린 뒤에 채운다.
        [[nodiscard]] std::unique_ptr<T, Deleter> Rent()
        {
            T* object{};
            if (!t_cacheGone) // 캐시가 이미 소멸했으면 새로 만든다(스레드 종료 중의 드문 경우)
            {
                auto& cache = t_cache;
                if (cache.count == 0)
                {
                    cache.count = PopBatch(cache.items.data()); // 비었으면 전역에서 한 묶음
                }
                if (cache.count > 0)
                {
                    object = cache.items[--cache.count];
                }
            }
            if (object == nullptr)
            {
                object = Create();
            }
#ifndef NDEBUG
            _rented.fetch_add(1);
#endif
            return std::unique_ptr<T, Deleter>(object);
        }

        // 지금 이 스레드의 캐시에 있는 객체 수. 테스트와 진단용(락 없음).
        [[nodiscard]] static std::size_t ThreadCachedCount() noexcept
        {
            return t_cacheGone ? 0 : t_cache.count;
        }
        // 전역 풀에 있는 객체 수. 테스트와 진단용(mutex를 잡는다).
        [[nodiscard]] std::size_t GlobalCachedCount()
        {
            const std::scoped_lock lock(_mutex);
            return _freeCount;
        }

#ifndef NDEBUG
        // 지금 빌려 가 있는 객체 수. Debug에서만 센다(BufferPool과 같은 이유).
        [[nodiscard]] std::ptrdiff_t RentedCount() const noexcept
        {
            return _rented.load();
        }
#endif

    private:
        // 객체 바로 뒤에 붙는 연결 칸. next: 같은 묶음의 다음, nextBatch·count: 묶음 첫 객체에만 쓴다.
        struct Links
        {
            Links* next{};
            Links* nextBatch{};
            std::size_t count{};
        };

        // 블록 하나 = [T][Links]. Links 자리는 T 뒤를 Links 정렬로 올린 곳.
        static constexpr std::size_t LinksOffset{(sizeof(T) + alignof(Links) - 1) / alignof(Links) * alignof(Links)};
        static constexpr std::size_t BlockSize{LinksOffset + sizeof(Links)};
        static constexpr std::align_val_t BlockAlign{std::max(alignof(T), alignof(Links))};

        struct ThreadCache
        {
            constexpr ThreadCache() = default;
            // 스레드가 끝나면 남은 객체를 전역으로 보낸다(안 보내면 누수).
            ~ThreadCache()
            {
                ObjectPool::Shared().PushBatch(items.data(), count);
                count = 0;
                t_cacheGone = true; // 이후 이 스레드의 Rent·반납은 캐시를 거치지 않는다(BufferPool R1과 같은 이유)
            }
            ThreadCache(const ThreadCache&) = delete;
            ThreadCache& operator=(const ThreadCache&) = delete;
            ThreadCache(ThreadCache&&) = delete;
            ThreadCache& operator=(ThreadCache&&) = delete;

            std::array<T*, CacheCount> items{};
            std::size_t count{};
        };

        constexpr ObjectPool() = default;
        ~ObjectPool() = default; // 프로그램이 끝날 때까지 산다. 들고 있던 객체는 OS가 회수한다

        // 블록을 할당해 T를 기본 생성하고 뒤에 Links를 놓는다. T 생성자가 던지면 블록을 돌려주고 그대로 던진다.
        static T* Create()
        {
            void* block = ::operator new(BlockSize, BlockAlign);
            T* object{};
            try
            {
                object = ::new (block) T();
            }
            catch (...)
            {
                ::operator delete(block, BlockAlign);
                throw;
            }
            ::new (static_cast<std::byte*>(block) + LinksOffset) Links{};
            return object;
        }

        static Links* LinksOf(T* object) noexcept
        {
            return std::launder(reinterpret_cast<Links*>(reinterpret_cast<std::byte*>(object) + LinksOffset));
        }
        static T* ObjectOf(Links* links) noexcept
        {
            return std::launder(reinterpret_cast<T*>(reinterpret_cast<std::byte*>(links) - LinksOffset));
        }

        // 반납: Reset() 후 이 스레드의 캐시로. 캐시가 꽉 찼으면 오래된 쪽(앞) 묶음 하나를 전역에 보낸다.
        void Return(T* object) noexcept
        {
#ifndef NDEBUG
            _rented.fetch_sub(1);
#endif
            object->Reset();
            if (t_cacheGone)
            {
                PushBatch(&object, 1); // 캐시가 이미 소멸했다: 전역으로 바로
                return;
            }
            auto& cache = t_cache;
            if (cache.count == CacheCount)
            {
                PushBatch(cache.items.data(), BatchCount);
                std::move(cache.items.begin() + static_cast<std::ptrdiff_t>(BatchCount), cache.items.end(),
                          cache.items.begin());
                cache.count -= BatchCount;
            }
            cache.items[cache.count++] = object;
        }

        // 묶음 하나를 전역에 넣는다. 락 밖에서 객체끼리 잇고, 락 안에서는 머리만 바꾼다.
        void PushBatch(T* const* items, const std::size_t count) noexcept
        {
            if (count == 0)
            {
                return;
            }
            Links* next{};
            for (auto i = count; i > 0; --i)
            {
                auto* links = LinksOf(items[i - 1]);
                links->next = next;
                next = links;
            }
            auto* head = next;
            head->count = count;

            const std::scoped_lock lock(_mutex);
            head->nextBatch = _batches;
            _batches = head;
            _freeCount += count;
        }

        // 묶음 하나를 꺼내 items에 채우고 개수를 돌려준다(최대 CacheCount). 전역이 비었으면 0.
        std::size_t PopBatch(T** items) noexcept
        {
            Links* head{};
            {
                const std::scoped_lock lock(_mutex);
                head = _batches;
                if (head == nullptr)
                {
                    return 0;
                }
                _batches = head->nextBatch;
                _freeCount -= head->count;
            }
            auto count = std::size_t{0};
            for (auto* links = head; links != nullptr; links = links->next)
            {
                items[count++] = ObjectOf(links);
            }
            return count;
        }

        static ObjectPool s_shared;              // 이 타입의 하나뿐인 풀(아래 constinit 정의)
        static thread_local ThreadCache t_cache; // 스레드마다 하나
        static thread_local bool t_cacheGone;    // 이 스레드의 캐시가 이미 소멸했는가(소멸자 없는 값)

        std::mutex _mutex{};
        Links* _batches{};        // _mutex가 지킨다. 묶음 목록의 머리
        std::size_t _freeCount{}; // _mutex가 지킨다. 모든 묶음의 객체 수 합
#ifndef NDEBUG
        std::atomic<std::ptrdiff_t> _rented{};
#endif
    };

    // constinit: 프로그램 시작 전에 만들어진다(첫 사용 검사 없음, 다른 전역보다 먼저).
    template <Poolable T>
    constinit ObjectPool<T> ObjectPool<T>::s_shared{};
    template <Poolable T>
    thread_local constinit typename ObjectPool<T>::ThreadCache ObjectPool<T>::t_cache{};
    template <Poolable T>
    thread_local constinit bool ObjectPool<T>::t_cacheGone = false;

    // ObjectPool<T>에서 빌린 객체 핸들. 소멸하면 풀로 반납한다. 복사는 안 되고 std::move로 넘긴다.
    // 직접 new 한 포인터로 만들면 안 된다(풀이 만든 블록만 반납할 수 있다). release()하면 풀로 돌아오지 않는다.
    template <Poolable T>
    using PooledPtr = std::unique_ptr<T, typename ObjectPool<T>::Deleter>;
} // namespace Memory
