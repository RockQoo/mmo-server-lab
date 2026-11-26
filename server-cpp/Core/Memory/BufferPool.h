#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>

namespace Memory
{
    // BufferPool에서 빌린 버퍼 하나. 소멸하면 풀로 자동 반납한다(RAII).
    // 복사는 막고 이동만 된다: 같은 버퍼를 두 곳이 쥐거나 두 번 반납할 수 없다.
    // 범위를 넘어가야 하면(IOCP 송신 등) std::move로 소유권을 넘긴다. 어느 스레드에서 반납해도 된다.
    //
    // 왜 Rent/Return 날 포인터가 아니라 핸들인가: 반납 빠뜨림(누수), 두 번 반납(두 곳이 같은 버퍼를 빌려 데이터가
    // 섞임), 크기를 틀리게 반납하는 실수를 막는다. 비용은 포인터 두 개(16바이트)이고 호출은 인라인된다.
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
        PooledBuffer(std::byte* data, const std::size_t size) noexcept;

        std::byte* _data{};
        std::size_t _size{};
    };

    // 송수신·직렬화 버퍼를 다시 쓰는 풀(2판). 프로세스에 하나(Shared)이고 프로그램이 끝날 때까지 산다.
    // 크기 등급 5개(256B, 1KB, 4KB, 16KB, 64KB) 중 요청 크기 이상인 가장 작은 등급을 준다.
    // 64KB보다 크면 풀에 두지 않고 그때 할당하고 반납할 때 바로 해제한다(드문 큰 패킷이 풀을 차지하지 않게).
    //
    // 구조(그림은 docs/journal/bufferpool.html)
    //   스레드 캐시  스레드마다 등급별로 반납된 빈 버퍼를 CacheCapacity개까지 들고 있다. 보통의 Rent·Return은 여기서
    //                끝난다(그 스레드만 만지므로 락·원자 연산이 없다).
    //   전역 풀      등급마다 mutex 하나 + 묶음 목록(1판 구조). 캐시가 넘치면 BatchSize개를 한 묶음으로 보내고,
    //                캐시가 비면 한 묶음을 가져온다. mutex는 묶음 하나에 한 번만 잡는다.
    //   스레드 종료  캐시에 남은 버퍼를 전역으로 보낸다.
    // 빌린 스레드와 반납하는 스레드가 달라도 된다(Lane이 빌리고 IOCP 스레드가 송신 완료 후 반납). 반납된 버퍼는 반납한
    // 스레드의 캐시로 간다.
    //
    // 1판(인스턴스, 등급마다 mutex, 호출마다 원자 카운터)은 스레드가 늘수록 느려져 이 구조로 바꿨다(측정은 journal).
    // 반납된 버퍼는 OS에 돌려주지 않는다(상한 없음).
    class BufferPool
    {
    public:
        static constexpr std::size_t MinSize{256};
        static constexpr std::size_t ClassCount{5}; // 등급마다 4배: 256, 1K, 4K, 16K, 64K
        static constexpr std::size_t MaxSize{MinSize << (2 * (ClassCount - 1))};
        static constexpr std::size_t CacheCapacity{16};            // 스레드 하나가 등급마다 들고 있는 빈 버퍼 수
        static constexpr std::size_t BatchSize{CacheCapacity / 2}; // 전역과 한 번에 주고받는 수

        // 프로세스에 하나뿐인 풀. 프로그램이 끝날 때까지 산다(스레드 캐시가 끝날 때 돌아올 곳이 늘 있다).
        [[nodiscard]] static BufferPool& Shared() noexcept;

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&&) = delete;
        BufferPool& operator=(BufferPool&&) = delete;

        // minSize 이상인 버퍼를 빌린다. 내 캐시 → 전역 묶음 → 새로 할당 순서로 찾는다. 내용은 비우지 않는다.
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

        // 지금 이 스레드의 캐시에 있는 size 등급 빈 버퍼 수. 테스트와 진단용(락 없음).
        [[nodiscard]] static std::size_t ThreadCachedCount(const std::size_t size) noexcept;
        // 전역 풀에 있는 size 등급 빈 버퍼 수. 테스트와 진단용(그 등급의 mutex를 잡는다).
        [[nodiscard]] std::size_t GlobalCachedCount(const std::size_t size);

#ifndef NDEBUG
        // 지금 빌려 가 있는 버퍼 수(풀 밖 큰 버퍼 포함). Debug에서만 센다(모든 스레드가 같은 카운터를 쓰면 경합이라
        // Release에서는 세지 않는다).
        [[nodiscard]] std::ptrdiff_t RentedCount() const noexcept
        {
            return _rented.load();
        }
#endif

    private:
        friend class PooledBuffer;
        friend struct ThreadCache;

        constexpr BufferPool() = default;
        ~BufferPool() = default; // 프로그램이 끝날 때까지 살고, 들고 있던 버퍼는 OS가 프로세스와 함께 회수한다

        void Return(std::byte* data, const std::size_t size) noexcept;

        // 묶음 하나를 전역에 넣는다/꺼낸다. 묶음은 버퍼 안에 적은 FreeNode로 이어진다(할당 없음).
        void PushBatch(const std::size_t classIndex, std::byte* const* items, const std::size_t count) noexcept;
        // 꺼낸 묶음을 items에 채우고 개수를 돌려준다(최대 CacheCapacity). 전역이 비었으면 0.
        std::size_t PopBatch(const std::size_t classIndex, std::byte** items) noexcept;

        [[nodiscard]] static std::size_t ClassIndex(const std::size_t size) noexcept;

        // 반납된 버퍼 맨 앞에 적는다. next: 같은 묶음의 다음 버퍼, nextBatch·count: 묶음 첫 버퍼에만 쓴다.
        struct FreeNode
        {
            FreeNode* next{};
            FreeNode* nextBatch{};
            std::size_t count{};
        };

        struct SizeClass
        {
            constexpr SizeClass() = default;
            ~SizeClass() = default;
            SizeClass(const SizeClass&) = delete; // mutex를 들고 있어 옮길 수 없다
            SizeClass& operator=(const SizeClass&) = delete;
            SizeClass(SizeClass&&) = delete;
            SizeClass& operator=(SizeClass&&) = delete;

            std::mutex mutex{};
            FreeNode* batches{};     // mutex가 지킨다. 묶음 목록의 머리
            std::size_t freeCount{}; // mutex가 지킨다. 모든 묶음의 버퍼 수 합
        };

        static BufferPool s_shared; // Shared()가 돌려주는 하나뿐인 풀(BufferPool.cpp, constinit)

        std::array<SizeClass, ClassCount> _classes{};
#ifndef NDEBUG
        std::atomic<std::ptrdiff_t> _rented{};
#endif
    };
} // namespace Memory
