#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include "ThreadTeam.h"

namespace Bench
{
    // 빌리는 스레드 N개 → 반납하는 스레드 M개 측정 틀(풀 벤치마크 공용, C# Bench/RentReturnGrid.cs 와 같은 방법).
    //
    // 실무 모양: IOCP 스레드 여러 개가 패킷 객체를 빌리고 Lane 하나가 반납(4:1), Lane 하나가 송신 버퍼를 빌리고 IOCP
    // 스레드 여러 개가 반납(1:4). 비율이 기울면 한쪽 스레드 캐시는 늘 넘치고 다른 쪽은 늘 비어 전역 풀을 계속 거친다.
    // 예전 CrossThread(독립된 1:1 쌍)는 이 압박을 못 보였다(2026-11-27 사용자 지적, PROGRESS 후순위).
    //
    // 넘기는 방법: (빌리는 쪽, 반납하는 쪽) 쌍마다 우편함 하나. 빌리는 쪽은 BlockSize 개를 빌려 블록 하나를 채우고,
    // 반납하는 쪽을 차례로 바꿔 가며(블록 번호 % M) 그 쌍의 우편함에 넣는다. 반납하는 쪽은 자기 열의 우편함 N개를 돌며
    // 블록을 받아 모두 반납한다. 넘기는 비용은 블록 하나에 원자 연산 몇 번이라, 측정되는 것은 거의 풀 비용이다(Lane
    // 으로 넘기면 Lane 비용이 풀 비용을 덮는다. decisions/002).
    //
    // 1:1 은 기준선이다: 건너가는 비용(전역 묶음 오가기)은 있지만 전역 mutex 다툼은 거의 없다. 1:1 → N:M 차이가 다툼
    // 비용이다.
    template <typename Handle, std::size_t BlockSize = 64>
    class RentReturnGrid
    {
    public:
        using Block = std::array<Handle, BlockSize>;

        RentReturnGrid(const int renters, const int returners)
            : _renters(renters), _returners(returners),
              _cells(std::make_unique<Cell[]>(static_cast<std::size_t>(renters) * static_cast<std::size_t>(returners))),
              _team(renters + returners)
        {
        }
        ~RentReturnGrid() = default;
        RentReturnGrid(const RentReturnGrid&) = delete;
        RentReturnGrid& operator=(const RentReturnGrid&) = delete;
        RentReturnGrid(RentReturnGrid&&) = delete;
        RentReturnGrid& operator=(RentReturnGrid&&) = delete;

        // 전체 totalOps 개를 빌리고 반납할 때까지 돌린다. rent() 는 Handle 하나를 빌려 돌려준다.
        // totalOps 는 N * M * BlockSize 로 나누어떨어져야 한다(쌍마다 같은 수의 블록).
        template <typename Rent>
        void Run(const std::int64_t totalOps, const Rent& rent)
        {
            const auto renters = static_cast<std::int64_t>(_renters);
            const auto returners = static_cast<std::int64_t>(_returners);
            const auto blockSize = static_cast<std::int64_t>(BlockSize);
            if (totalOps % (renters * returners * blockSize) != 0)
            {
                std::terminate(); // 측정 조건 실수
            }
            const auto blocksPerPair = totalOps / (renters * returners * blockSize);

            _team.Run(
                [this, &rent, blocksPerPair](const int t)
                {
                    if (t < _renters)
                    {
                        RunRenter(t, blocksPerPair, rent);
                    }
                    else
                    {
                        RunReturner(t - _renters, blocksPerPair);
                    }
                });
        }

    private:
        // 쌍 하나. 블록 두 개를 번갈아 채운다: 우편함이 비었다는 것은 이전에 넣은 블록을 다 반납했다는 뜻이라, 다음
        // 블록을 넣기 전에 우편함이 비기를 기다리면 지금 채운 블록과 반납 중인 블록이 겹치지 않는다.
        // 우편함 앞뒤를 띄워 이웃 쌍의 블록·우편함과 같은 캐시 줄에 놓이지 않게 한다(alignas 는 C4324 경고라 패딩).
        struct Cell
        {
            Cell() = default;
            ~Cell() = default;
            Cell(const Cell&) = delete;
            Cell& operator=(const Cell&) = delete;
            Cell(Cell&&) = delete;
            Cell& operator=(Cell&&) = delete;

            std::array<std::byte, 128> paddingBefore{};
            std::atomic<Block*> mailbox{}; // 비었으면 nullptr
            std::array<std::byte, 128> paddingAfter{};
            std::array<Block, 2> blocks{};
        };

        Cell& CellAt(const int renter, const int returner) noexcept
        {
            return _cells[static_cast<std::size_t>(renter) * static_cast<std::size_t>(_returners) +
                          static_cast<std::size_t>(returner)];
        }

        template <typename Rent>
        void RunRenter(const int renter, const std::int64_t blocksPerPair, const Rent& rent)
        {
            auto next = std::vector<std::size_t>(static_cast<std::size_t>(_returners)); // 쌍마다 다음에 채울 블록
            const auto blocks = blocksPerPair * _returners;
            for (auto b = std::int64_t{0}; b < blocks; ++b)
            {
                const auto returner = static_cast<int>(b % _returners); // 반납하는 쪽을 차례로 바꾼다
                auto& cell = CellAt(renter, returner);
                auto& slot = next[static_cast<std::size_t>(returner)];
                auto& block = cell.blocks[slot];
                for (auto& handle : block)
                {
                    handle = rent();
                }
                while (cell.mailbox.load(std::memory_order_acquire) != nullptr)
                {
                    std::this_thread::yield();
                }
                cell.mailbox.store(&block, std::memory_order_release);
                slot ^= 1U;
            }
        }

        void RunReturner(const int returner, const std::int64_t blocksPerPair)
        {
            const auto expected = blocksPerPair * _renters;
            auto received = std::int64_t{0};
            while (received < expected)
            {
                auto progressed = false;
                for (auto renter = 0; renter < _renters; ++renter)
                {
                    auto& cell = CellAt(renter, returner);
                    auto* const block = cell.mailbox.load(std::memory_order_acquire);
                    if (block == nullptr)
                    {
                        continue;
                    }
                    for (auto& handle : *block)
                    {
                        handle = Handle{}; // 반납(이동 대입이 들고 있던 것을 돌려준다)
                    }
                    cell.mailbox.store(nullptr, std::memory_order_release);
                    ++received;
                    progressed = true;
                }
                if (!progressed)
                {
                    std::this_thread::yield();
                }
            }
        }

        int _renters;
        int _returners;
        std::unique_ptr<Cell[]> _cells;
        ThreadTeam _team; // 마지막 멤버: 가장 먼저 소멸(작업자 join)한다
    };
} // namespace Bench
