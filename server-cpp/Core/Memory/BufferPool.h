#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>

namespace Memory
{
    class BufferPool;

    // BufferPool에서 빌린 버퍼 하나. 소멸하면 풀로 자동 반납한다(RAII).
    // 복사는 막고 이동만 된다: 같은 버퍼를 두 곳이 쥐거나 두 번 반납할 수 없다.
    // 범위를 넘어가야 하면(IOCP 송신 등) std::move로 소유권을 넘긴다.
    //
    // 왜 Rent/Return 날 포인터가 아니라 핸들인가: 반납 빠뜨림(누수), 두 번 반납(두 곳이 같은 버퍼를 빌려 데이터가
    // 섞임), 크기를 틀리게 반납하는 실수를 막는다. 비용은 포인터 세 개(24바이트)이고 호출은 인라인된다.
    // C#은 이 핸들을 만들지 않는다(ArrayPool<byte>.Shared + try/finally Return, 이유는 docs/naming.md).
    class PooledBuffer
    {
    public:
        PooledBuffer() noexcept = default; // 빈 핸들(아무것도 빌리지 않음)
        ~PooledBuffer();

        PooledBuffer(const PooledBuffer&) = delete;
        PooledBuffer& operator=(const PooledBuffer&) = delete;
        PooledBuffer(PooledBuffer&& other) noexcept;
        PooledBuffer& operator=(PooledBuffer&& other) noexcept; // 들고 있던 버퍼는 먼저 반납한다

        [[nodiscard]] std::byte* Data() const noexcept
        {
            return _data;
        }
        // 실제 크기(등급 크기). Rent에 준 크기 이상이다.
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _size;
        }
        [[nodiscard]] std::span<std::byte> Span() const noexcept
        {
            return {_data, _size};
        }
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return _data == nullptr;
        }

        // 소멸을 기다리지 않고 지금 반납한다. 그 뒤에는 빈 핸들이다. 빈 핸들에 불러도 된다.
        void Reset() noexcept;

    private:
        friend class BufferPool;
        PooledBuffer(BufferPool* pool, std::byte* data, const std::size_t size) noexcept;

        BufferPool* _pool{};
        std::byte* _data{};
        std::size_t _size{};
    };

    // 송수신·직렬화 버퍼를 다시 쓰는 풀. 크기 등급 5개(256B, 1KB, 4KB, 16KB, 64KB) 중 요청 크기 이상인
    // 가장 작은 등급을 준다(이동 패킷은 작고 스냅샷은 커서 등급 하나로는 낭비가 크다).
    // 64KB보다 크면 풀에 두지 않고 그때 할당하고 반납할 때 바로 해제한다(드문 큰 패킷이 풀을 차지하지 않게).
    //
    // 스레드: 어느 스레드에서 빌리고 반납해도 된다. 등급마다 mutex 하나(1판). 스레드별 캐시를 둔 2판과
    // 벤치마크로 비교한다(PROGRESS.md). 반납된 버퍼는 풀이 사라질 때까지 들고 있다(상한 없음, 1판).
    //
    // 풀은 빌려 간 버퍼보다 오래 살아야 한다. 빌려 간 버퍼가 남은 채 풀이 사라지면 ASSERT.
    class BufferPool
    {
    public:
        static constexpr std::size_t MinSize{256};
        static constexpr std::size_t ClassCount{5}; // 등급마다 4배: 256, 1K, 4K, 16K, 64K
        static constexpr std::size_t MaxSize{MinSize << (2 * (ClassCount - 1))};

        BufferPool() = default;
        ~BufferPool();

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&&) = delete;
        BufferPool& operator=(BufferPool&&) = delete;

        // minSize 이상인 버퍼를 빌린다. 반납된 버퍼가 있으면 그것을, 없으면 새로 할당한다. 내용은 비우지 않는다.
        [[nodiscard]] PooledBuffer Rent(const std::size_t minSize);

        // minSize를 빌리면 받는 크기(등급 크기, MaxSize보다 크면 minSize 그대로).
        [[nodiscard]] static constexpr std::size_t SizeFor(const std::size_t minSize) noexcept
        {
            auto size = MinSize;
            while (size < minSize && size < MaxSize)
            {
                size <<= 2;
            }
            return size < minSize ? minSize : size;
        }

        // 지금 빌려 가 있는 버퍼 수(풀 밖 큰 버퍼 포함).
        [[nodiscard]] std::size_t RentedCount() const noexcept;
        // 반납되어 풀에 쌓여 있는 버퍼 수(모든 등급 합).
        [[nodiscard]] std::size_t CachedCount();

    private:
        friend class PooledBuffer;
        void Return(std::byte* data, const std::size_t size) noexcept;

        [[nodiscard]] static std::size_t ClassIndex(const std::size_t size) noexcept;

        // 반납된 버퍼 맨 앞에 다음 반납 버퍼의 주소를 적어 잇는다(free list). 반납할 때 할당이 없어
        // noexcept를 지킬 수 있다(vector에 쌓으면 push_back이 할당하다 던질 수 있다).
        struct FreeNode
        {
            FreeNode* next{};
        };

        struct SizeClass
        {
            SizeClass() = default;
            ~SizeClass() = default;
            SizeClass(const SizeClass&) = delete; // mutex를 들고 있어 옮길 수 없다
            SizeClass& operator=(const SizeClass&) = delete;
            SizeClass(SizeClass&&) = delete;
            SizeClass& operator=(SizeClass&&) = delete;

            std::mutex mutex{};
            FreeNode* head{};        // mutex가 지킨다
            std::size_t freeCount{}; // mutex가 지킨다
        };

        std::array<SizeClass, ClassCount> _classes{};
        std::atomic<std::size_t> _rented{};
    };
} // namespace Memory
