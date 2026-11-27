#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "Base/Assert.h"

// 비교 전용 타이머 휠(한 단계 해시 휠). Scheduling::TimerHeap 과 성능을 비교하려고 Bench 에만 둔다(Core 에 없다).
//
// 모양: 칸 slotCount 개(2의 거듭제곱)가 고리로 돌고, 칸 하나 = tick 시간. 한 바퀴 = tick × slotCount.
//   - 일은 "몇 번째 틱에 실행"(dueTick)을 들고, dueTick % slotCount 칸의 이중 연결 목록에 들어간다.
//   - 한 바퀴보다 먼 일도 같은 칸에 들어간다. 칸을 지날 때 dueTick 이 아직이면 그대로 둔다(= 남은 바퀴 수를 세는 것과
//     같다). 그래서 오래 뒤 일(버프 30분 등)은 바퀴마다 한 번씩 훑인다. 이 비용을 벤치에서 본다.
//   - 일 칸은 배열(_nodes)에 두고 빈 칸은 다시 쓴다. id = (세대 << 32) | 배열 번호. 세대는 칸을 비울 때마다 올라서
//     다시 쓴 칸을 예전 id 로 지우지 못한다. Cancel 은 목록에서 바로 빼므로 O(1)(TimerHeap 처럼 표시만 하지 않는다).
//
// TimerHeap 과 다른 점(비교할 때 알아 둘 것)
//   - 시각을 tick 단위로 올림한다. 일찍 실행되지는 않지만 최대 tick 만큼 늦을 수 있다.
//   - 같은 칸 안의 실행 순서는 넣은 순서가 아니다(목록 앞에 넣는다). TimerHeap 은 시각 순서, 같으면 넣은 순서.
//   - 주기 일은 TimerHeap 과 같은 고정 간격(놓친 칸은 건너뛴다).
//   - 일 안에서 Add·Cancel 해도 된다. 다만 일 안에서 이번 틱이나 지난 시각으로 Add 한 일은 다음 틱에 실행된다
//     (TimerHeap 은 같은 RunDue 에서 실행).
//   - 일이 예외를 던지는 경우는 다루지 않는다(벤치의 일은 던지지 않는다).
//   - 스레드 안전하지 않다(TimerHeap 과 같다).
namespace Bench
{
    class TimerWheel
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using Duration = Clock::duration;
        using Job = std::move_only_function<void()>;

        TimerWheel(const TimePoint start, const Duration tick, const std::size_t slotCount)
            : _start(start), _tick(tick), _mask(slotCount - 1), _heads(slotCount, Nil)
        {
            ASSERT(tick > Duration::zero(), "TimerWheel: tick must be positive");
            ASSERT(slotCount > 0 && (slotCount & (slotCount - 1)) == 0, "TimerWheel: slotCount must be a power of 2");
        }

        // due 에 job 을 실행한다. period 가 0 보다 크면 그 뒤로 period 마다(tick 단위로 올림).
        // 발급하는 id 는 0 이 아니다.
        uint64_t Add(const TimePoint due, Job job, const Duration period = Duration::zero())
        {
            ASSERT(job != nullptr, "TimerWheel::Add: empty job");
            ASSERT(period >= Duration::zero(), "TimerWheel::Add: negative period");
            auto index = uint32_t{};
            if (_free.empty())
            {
                index = static_cast<uint32_t>(_nodes.size());
                _nodes.emplace_back();
            }
            else
            {
                index = _free.back();
                _free.pop_back();
            }
            auto& node = _nodes[index];
            node.job = std::move(job);
            node.dueTick = std::max(CeilTick(due - _start), _nextTick);
            node.periodTicks = period > Duration::zero() ? std::max(CeilTick(period), int64_t{1}) : int64_t{0};
            Link(index);
            ++_size;
            return (uint64_t{node.generation} << 32) | index;
        }

        // 아직 실행 전(주기 일은 다음 실행 전)이면 지우고 true. 이미 끝났거나 없는 id 면 false.
        bool Cancel(const uint64_t id)
        {
            const auto index = static_cast<uint32_t>(id & 0xFFFF'FFFF);
            const auto generation = static_cast<uint32_t>(id >> 32);
            if (index >= _nodes.size() || _nodes[index].generation != generation)
            {
                return false;
            }
            auto& node = _nodes[index];
            if (node.state == State::Linked)
            {
                Unlink(index);
                Release(index);
                return true;
            }
            if (node.state == State::Pending)
            {
                Release(index); // 이번 칸에서 실행하려고 꺼내 둔 일: RunSlot 이 세대를 보고 건너뛴다
                return true;
            }
            if (node.state == State::Running)
            {
                node.state = State::Canceled; // 실행 중인 주기 일이 자기 자신을 지움: 끝난 뒤 다시 넣지 않는다
                return true;
            }
            return false;
        }

        // now 까지 시각이 된 틱을 차례로 처리한다. 실행한 일 수.
        std::size_t RunDue(const TimePoint now)
        {
            ASSERT(!_running, "TimerWheel::RunDue called from inside a job");
            const auto offset = now - _start;
            const auto target = offset < Duration::zero() ? int64_t{-1} : int64_t{offset / _tick};
            auto ran = std::size_t{0};
            _running = true;
            while (_nextTick <= target)
            {
                if (_size == 0)
                {
                    _nextTick = target + 1; // 빈 휠: 빈 칸을 하나씩 지나지 않는다
                    break;
                }
                const auto tick = _nextTick++; // 먼저 올린다: 일 안에서 Add 한 일은 다음 틱 이후로 들어간다
                ran += RunSlot(tick);
            }
            _running = false;
            return ran;
        }

        // 남은 일 수(지운 것 제외)
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _size;
        }

    private:
        static constexpr uint32_t Nil = 0xFFFF'FFFF;

        enum class State : uint8_t
        {
            Free,     // 빈 칸(다시 쓸 수 있다)
            Linked,   // 칸 목록에 있다
            Pending,  // 이번 칸에서 실행하려고 목록에서 꺼냈다
            Running,  // 주기 일을 실행하는 중
            Canceled, // 주기 일 실행 중에 지워졌다
        };

        // 일(move_only_function)이 복사되지 않아 Node 도 옮기기만 한다
        struct Node
        {
            Node() = default;
            ~Node() = default;
            Node(const Node&) = delete;
            Node& operator=(const Node&) = delete;
            Node(Node&&) noexcept = default;
            Node& operator=(Node&&) noexcept = default;

            Job job;
            int64_t dueTick{};
            int64_t periodTicks{}; // 0 = 한 번
            uint32_t prev{Nil};
            uint32_t next{Nil};
            uint32_t generation{1};
            State state{State::Free};
        };

        struct PendingRun
        {
            uint32_t index;
            uint32_t generation;
        };

        // 시간을 틱 수로 올림(0 이하는 0)
        [[nodiscard]] int64_t CeilTick(const Duration duration) const noexcept
        {
            if (duration <= Duration::zero())
            {
                return 0;
            }
            return int64_t{(duration + _tick - Duration{1}) / _tick};
        }

        uint32_t& HeadOf(const int64_t tick) noexcept
        {
            return _heads[static_cast<std::size_t>(tick) & _mask];
        }

        void Link(const uint32_t index)
        {
            auto& node = _nodes[index];
            auto& head = HeadOf(node.dueTick);
            node.prev = Nil;
            node.next = head;
            if (head != Nil)
            {
                _nodes[head].prev = index;
            }
            head = index;
            node.state = State::Linked;
        }

        void Unlink(const uint32_t index)
        {
            const auto& node = _nodes[index];
            if (node.prev != Nil)
            {
                _nodes[node.prev].next = node.next;
            }
            else
            {
                HeadOf(node.dueTick) = node.next;
            }
            if (node.next != Nil)
            {
                _nodes[node.next].prev = node.prev;
            }
        }

        // 칸을 비운다. 세대를 올려 예전 id 를 무효로 만든다(0 은 건너뛴다: id 가 0 이 되지 않게).
        void Release(const uint32_t index)
        {
            auto& node = _nodes[index];
            node.job = nullptr;
            node.state = State::Free;
            if (++node.generation == 0)
            {
                node.generation = 1;
            }
            _free.push_back(index);
            --_size;
        }

        // 틱 하나: 칸 목록에서 시각이 된 일만 꺼내 두고(목록을 도는 중에 일이 Cancel 해도 안전하게) 차례로 실행한다.
        std::size_t RunSlot(const int64_t tick)
        {
            _pending.clear();
            for (auto index = HeadOf(tick); index != Nil;)
            {
                auto& node = _nodes[index];
                const auto next = node.next;
                if (node.dueTick <= tick)
                {
                    Unlink(index);
                    node.state = State::Pending;
                    _pending.push_back({.index = index, .generation = node.generation});
                }
                index = next; // 아직 바퀴가 남은 일은 그대로 둔다
            }
            auto ran = std::size_t{0};
            for (const auto& run : _pending)
            {
                // 일 안의 Add 가 _nodes 를 늘릴 수 있어 참조를 오래 들고 있지 않는다
                if (_nodes[run.index].generation != run.generation || _nodes[run.index].state != State::Pending)
                {
                    continue; // 앞의 일이 지웠다
                }
                auto job = std::move(_nodes[run.index].job);
                if (_nodes[run.index].periodTicks == 0)
                {
                    Release(run.index);
                    job();
                }
                else
                {
                    _nodes[run.index].state = State::Running;
                    job();
                    auto& node = _nodes[run.index];
                    if (node.state == State::Canceled)
                    {
                        Release(run.index);
                    }
                    else
                    {
                        // 고정 간격. 여러 칸을 놓쳤으면 이번 틱 다음 칸으로 건너뛴다(한 번만 실행)
                        node.dueTick += ((tick - node.dueTick) / node.periodTicks + 1) * node.periodTicks;
                        node.job = std::move(job);
                        Link(run.index);
                    }
                }
                ++ran;
            }
            return ran;
        }

        TimePoint _start;
        Duration _tick;
        std::size_t _mask;
        std::vector<uint32_t> _heads; // 칸마다 목록 머리(Nil = 빈 칸)
        std::vector<Node> _nodes{};
        std::vector<uint32_t> _free{};
        std::vector<PendingRun> _pending{};
        int64_t _nextTick{0}; // 다음에 처리할 틱
        std::size_t _size{0};
        bool _running{};
    };
} // namespace Bench
