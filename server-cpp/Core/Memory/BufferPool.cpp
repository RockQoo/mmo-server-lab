#include "Memory/BufferPool.h"

#include <new>
#include <utility>

namespace Memory
{
    // ---- PooledBuffer

    PooledBuffer::PooledBuffer(std::byte* data, const std::size_t size) noexcept : _data(data), _size(size)
    {
    }

    PooledBuffer::~PooledBuffer()
    {
        Reset();
    }

    PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
        : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0))
    {
    }

    PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            _data = std::exchange(other._data, nullptr);
            _size = std::exchange(other._size, 0);
        }
        return *this;
    }

    void PooledBuffer::Reset() noexcept
    {
        if (_data != nullptr)
        {
            BufferPool::Shared().Return(std::exchange(_data, nullptr), std::exchange(_size, 0));
        }
    }

    // ---- 전역 풀 하나

    // constinit: 프로그램 시작 전에 만들어진다(다른 전역보다 먼저, 첫 사용 검사 없음). 소멸은 가장 늦다.
    constinit BufferPool BufferPool::s_shared{};

    BufferPool& BufferPool::Shared() noexcept
    {
        return s_shared;
    }

    // ---- 스레드 캐시

    // 스레드마다 등급별로 반납된 빈 버퍼를 들고 있다. 그 스레드만 만지므로 락이 없다.
    struct ThreadCache
    {
        struct Slot
        {
            std::array<std::byte*, BufferPool::CacheCapacity> items{};
            std::size_t count{};
        };

        ThreadCache() = default;
        // 스레드가 끝나면 남은 버퍼를 전역으로 보낸다(안 보내면 누수).
        ~ThreadCache()
        {
            for (auto index = std::size_t{0}; index < BufferPool::ClassCount; ++index)
            {
                auto& slot = slots[index];
                if (slot.count > 0)
                {
                    BufferPool::Shared().PushBatch(index, slot.items.data(), slot.count);
                    slot.count = 0;
                }
            }
        }
        ThreadCache(const ThreadCache&) = delete;
        ThreadCache& operator=(const ThreadCache&) = delete;
        ThreadCache(ThreadCache&&) = delete;
        ThreadCache& operator=(ThreadCache&&) = delete;

        std::array<Slot, BufferPool::ClassCount> slots{};
    };

    thread_local ThreadCache t_cache;

    // ---- BufferPool

    // 반납된 버퍼 안에 FreeNode를 적으므로 가장 작은 버퍼도 그 크기·정렬을 담아야 한다
    // (new std::byte[]는 __STDCPP_DEFAULT_NEW_ALIGNMENT__로 정렬된다).
    static_assert(BufferPool::MinSize >= 3 * sizeof(void*) && __STDCPP_DEFAULT_NEW_ALIGNMENT__ >= alignof(void*));
    static_assert(BufferPool::BatchSize > 0 && BufferPool::BatchSize <= BufferPool::CacheCapacity);

    PooledBuffer BufferPool::Rent(const std::size_t minSize)
    {
        const auto size = SizeFor(minSize);
        std::byte* data{};
        if (size <= MaxSize)
        {
            const auto index = ClassIndex(size);
            auto& slot = t_cache.slots[index];
            if (slot.count == 0)
            {
                slot.count = PopBatch(index, slot.items.data()); // 비었으면 전역에서 한 묶음
            }
            if (slot.count > 0)
            {
                data = slot.items[--slot.count];
            }
        }
        if (data == nullptr)
        {
            data = new std::byte[size]; // 캐시도 전역도 비었거나 64KB 초과
        }
#ifndef NDEBUG
        _rented.fetch_add(1);
#endif
        return PooledBuffer(data, size);
    }

    void BufferPool::Return(std::byte* data, const std::size_t size) noexcept
    {
#ifndef NDEBUG
        _rented.fetch_sub(1);
#endif
        if (size > MaxSize)
        {
            delete[] data; // 풀 밖 큰 버퍼
            return;
        }
        const auto index = ClassIndex(size);
        auto& slot = t_cache.slots[index];
        if (slot.count == CacheCapacity)
        {
            // 넘친다: 오래된 쪽(앞) BatchSize개를 한 묶음으로 전역에 보내고 나머지를 앞으로 당긴다.
            PushBatch(index, slot.items.data(), BatchSize);
            std::move(slot.items.begin() + BatchSize, slot.items.end(), slot.items.begin());
            slot.count -= BatchSize;
        }
        slot.items[slot.count++] = data;
    }

    void BufferPool::PushBatch(const std::size_t classIndex, std::byte* const* items, const std::size_t count) noexcept
    {
        if (count == 0)
        {
            return;
        }
        // 락 밖에서 버퍼끼리 잇는다. 락 안에서는 머리만 바꾼다.
        FreeNode* next{};
        for (auto i = count; i > 0; --i)
        {
            next = new (items[i - 1]) FreeNode{next, nullptr, 0};
        }
        auto* head = next;
        head->count = count;

        auto& sizeClass = _classes[classIndex];
        const std::scoped_lock lock(sizeClass.mutex);
        head->nextBatch = sizeClass.batches;
        sizeClass.batches = head;
        sizeClass.freeCount += count;
    }

    std::size_t BufferPool::PopBatch(const std::size_t classIndex, std::byte** items) noexcept
    {
        FreeNode* head{};
        {
            auto& sizeClass = _classes[classIndex];
            const std::scoped_lock lock(sizeClass.mutex);
            head = sizeClass.batches;
            if (head == nullptr)
            {
                return 0;
            }
            sizeClass.batches = head->nextBatch;
            sizeClass.freeCount -= head->count;
        }
        // 락 밖에서 묶음을 풀어 캐시에 담는다.
        auto count = std::size_t{0};
        for (auto* node = head; node != nullptr; node = node->next)
        {
            items[count++] = reinterpret_cast<std::byte*>(node); // FreeNode는 소멸자가 없다(포인터·정수뿐)
        }
        return count;
    }

    std::size_t BufferPool::ThreadCachedCount(const std::size_t size) noexcept
    {
        const auto classSize = SizeFor(size);
        return classSize > MaxSize ? 0 : t_cache.slots[ClassIndex(classSize)].count;
    }

    std::size_t BufferPool::GlobalCachedCount(const std::size_t size)
    {
        const auto classSize = SizeFor(size);
        if (classSize > MaxSize)
        {
            return 0;
        }
        auto& sizeClass = _classes[ClassIndex(classSize)];
        const std::scoped_lock lock(sizeClass.mutex);
        return sizeClass.freeCount;
    }

    std::size_t BufferPool::ClassIndex(const std::size_t size) noexcept
    {
        auto index = std::size_t{0};
        for (auto classSize = MinSize; classSize < size; classSize <<= 2)
        {
            ++index;
        }
        return index;
    }
} // namespace Memory
