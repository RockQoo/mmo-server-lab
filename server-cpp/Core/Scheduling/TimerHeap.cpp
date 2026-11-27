#include "Scheduling/TimerHeap.h"

#include <utility>
#include <vector>

#include "Base/Assert.h"

namespace Scheduling
{
    namespace
    {
        // 지운 칸이 이보다 적으면 다시 만들지 않는다(작은 힙은 치우는 비용이 더 크다)
        constexpr std::size_t MinCompactSize = 64;
    } // namespace

    TimerId TimerHeap::Add(const TimePoint due, Job job, const Duration period)
    {
        ASSERT(job != nullptr, "TimerHeap::Add: empty job");
        ASSERT(period >= Duration::zero(), "TimerHeap::Add: negative period");
        const auto id = _nextId++;
        _timers.try_emplace(id, due, period, std::move(job));
        _heap.push({.due = due, .id = id});
        return TimerId{id};
    }

    bool TimerHeap::Cancel(const TimerId id)
    {
        if (id.Value() == _runningId && _runningId != 0)
        {
            // 실행 중인 일이 자기 자신을 지움: 일은 RunDue 가 들고 있으므로 끝난 뒤 다시 넣지 않게 표시만 한다.
            // 한 번짜리 일은 이미 실행됐으니 지울 것이 없다(false).
            const auto first = !_runningCanceled;
            _runningCanceled = true;
            return first && _runningPeriodic;
        }
        if (_timers.erase(id.Value()) == 0)
        {
            return false;
        }
        CompactIfMostlyStale();
        return true;
    }

    std::size_t TimerHeap::RunDue(const TimePoint now)
    {
        ASSERT(_runningId == 0, "TimerHeap::RunDue called from inside a job");
        std::size_t ran = 0;
        while (!_heap.empty() && _heap.top().due <= now)
        {
            const auto slot = _heap.top();
            _heap.pop();
            auto found = _timers.find(slot.id);
            if (found == _timers.end() || found->second.due != slot.due)
            {
                continue; // 지운 일, 또는 이미 다른 시각으로 다시 들어간 칸
            }
            // 일을 목록에서 꺼내 실행한다: 일 안에서 Add 가 _timers 를 바꿔도(rehash) 꺼낸 일은 안전하다
            auto timer = std::move(found->second);
            _timers.erase(found);
            _runningId = slot.id;
            _runningPeriodic = timer.period > Duration::zero();
            _runningCanceled = false;
            // 일이 예외를 던져도 _runningId 를 되돌린다(그 일은 주기 일이어도 다시 넣지 않는다)
            try
            {
                timer.job();
            }
            catch (...)
            {
                _runningId = 0;
                throw;
            }
            _runningId = 0;
            ++ran;
            if (timer.period > Duration::zero() && !_runningCanceled)
            {
                // 고정 간격: 예정 시각 기준으로 다음 칸. 여러 칸을 놓쳤으면 now 다음 칸으로 건너뛴다(한 번만 실행)
                const auto missed = (now - timer.due) / timer.period + 1;
                timer.due += missed * timer.period;
                _heap.push({.due = timer.due, .id = slot.id});
                _timers.emplace(slot.id, std::move(timer));
            }
        }
        return ran;
    }

    std::optional<TimerHeap::TimePoint> TimerHeap::NextDue()
    {
        PopStale();
        if (_heap.empty())
        {
            return std::nullopt;
        }
        return _heap.top().due;
    }

    void TimerHeap::PopStale()
    {
        while (!_heap.empty())
        {
            const auto& top = _heap.top();
            const auto found = _timers.find(top.id);
            if (found != _timers.end() && found->second.due == top.due)
            {
                return;
            }
            _heap.pop();
        }
    }

    // 지운 칸이 남은 일보다 많으면 남은 일로 힙을 다시 만든다. 오래 뒤 일(버프 30분 등)을 많이 지울 때 힙이 그 칸들로
    // 계속 커지는 것을 막는다. 비용 O(n), 지운 칸이 절반을 넘을 때만이라 나눠 보면 Cancel 하나에 O(1).
    void TimerHeap::CompactIfMostlyStale()
    {
        if (_heap.size() < MinCompactSize || _heap.size() <= 2 * _timers.size())
        {
            return;
        }
        std::vector<Slot> slots;
        slots.reserve(_timers.size());
        for (const auto& [id, timer] : _timers)
        {
            slots.push_back({.due = timer.due, .id = id});
        }
        _heap = std::priority_queue<Slot>{std::less<Slot>{}, std::move(slots)};
    }
} // namespace Scheduling
