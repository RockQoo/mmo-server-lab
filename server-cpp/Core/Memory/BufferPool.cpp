#include "Memory/BufferPool.h"

#include <new>
#include <utility>

#include "Base/Assert.h"

namespace Memory
{
    // ---- PooledBuffer

    PooledBuffer::PooledBuffer(BufferPool* pool, std::byte* data, const std::size_t size) noexcept
        : _pool(pool), _data(data), _size(size)
    {
    }

    PooledBuffer::~PooledBuffer()
    {
        Reset();
    }

    PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
        : _pool(std::exchange(other._pool, nullptr)), _data(std::exchange(other._data, nullptr)),
          _size(std::exchange(other._size, 0))
    {
    }

    PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            _pool = std::exchange(other._pool, nullptr);
            _data = std::exchange(other._data, nullptr);
            _size = std::exchange(other._size, 0);
        }
        return *this;
    }

    void PooledBuffer::Reset() noexcept
    {
        if (_data != nullptr)
        {
            _pool->Return(std::exchange(_data, nullptr), std::exchange(_size, 0));
            _pool = nullptr;
        }
    }

    // ---- BufferPool

    // 반납된 버퍼 안에 FreeNode를 적으므로 가장 작은 버퍼도 그 크기·정렬을 담아야 한다
    // (new std::byte[]는 __STDCPP_DEFAULT_NEW_ALIGNMENT__로 정렬된다).
    static_assert(BufferPool::MinSize >= sizeof(void*) && __STDCPP_DEFAULT_NEW_ALIGNMENT__ >= alignof(void*));

    BufferPool::~BufferPool()
    {
        ASSERT(_rented.load() == 0, "BufferPool destroyed while buffers are still rented");
        for (auto& sizeClass : _classes)
        {
            while (sizeClass.head != nullptr)
            {
                auto* node = sizeClass.head;
                sizeClass.head = node->next;
                delete[] reinterpret_cast<std::byte*>(node); // FreeNode는 소멸자가 없다(포인터 하나)
            }
        }
    }

    PooledBuffer BufferPool::Rent(const std::size_t minSize)
    {
        const auto size = SizeFor(minSize);
        std::byte* data{};
        if (size <= MaxSize)
        {
            auto& sizeClass = _classes[ClassIndex(size)];
            const std::scoped_lock lock(sizeClass.mutex);
            if (auto* node = sizeClass.head; node != nullptr)
            {
                sizeClass.head = node->next;
                --sizeClass.freeCount;
                data = reinterpret_cast<std::byte*>(node);
            }
        }
        if (data == nullptr)
        {
            data = new std::byte[size]; // 락 밖에서 할당한다
        }
        _rented.fetch_add(1, std::memory_order_relaxed);
        return PooledBuffer(this, data, size);
    }

    void BufferPool::Return(std::byte* data, const std::size_t size) noexcept
    {
        _rented.fetch_sub(1, std::memory_order_relaxed);
        if (size > MaxSize)
        {
            delete[] data; // 풀 밖 큰 버퍼
            return;
        }
        auto& sizeClass = _classes[ClassIndex(size)];
        const std::scoped_lock lock(sizeClass.mutex);
        sizeClass.head = new (data) FreeNode{sizeClass.head}; // 버퍼 맨 앞에 적는다(할당 없음)
        ++sizeClass.freeCount;
    }

    std::size_t BufferPool::RentedCount() const noexcept
    {
        return _rented.load(std::memory_order_relaxed);
    }

    std::size_t BufferPool::CachedCount()
    {
        auto count = std::size_t{0};
        for (auto& sizeClass : _classes)
        {
            const std::scoped_lock lock(sizeClass.mutex);
            count += sizeClass.freeCount;
        }
        return count;
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
