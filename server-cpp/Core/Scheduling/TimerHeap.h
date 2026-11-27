#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>

#include "Base/StrongId.h"

// 시각 순서로 정리된 약속 목록("언제 무엇을"). 시계도 스레드도 없다: 부르는 쪽이 "지금"을 넘기면 그때까지 된 일을
// 실행한다. 쓰는 곳 두 가지(docs/journal/threading.html):
//   - 존 틱: 틱마다 RunDue(틱 시각)를 존 스레드에서 직접 부른다(버프 만료, 리스폰). Post 가 필요 없다.
//   - Scheduler: 타이머 스레드가 NextDue() 까지 자다가 RunDue(지금)을 부르고, 일은 담당 Lane 에 Post 한다.
// 테스트는 기다리지 않고 시각을 직접 넣는다.
//
//     TimerHeap heap;
//     const auto t0 = TimerHeap::Clock::now();
//     heap.Add(t0 + 3s, [] { ... });                 // 3초 뒤 한 번
//     const auto id = heap.Add(t0 + 5s, [] { ... }, 5s); // 5초 뒤부터 5초마다
//     heap.RunDue(t0 + 6s);                          // 3초·5초 일 실행, 주기 일은 10초로 다시 들어감
//     heap.Cancel(id);
//
// 규칙
//   - 같은 시각이면 넣은 순서대로 실행한다(결과가 늘 같다).
//   - 주기 일은 "고정 간격": 다음 = 이전 예정 시각 + 주기. 늦게 실행돼도 다음 예정은 밀리지 않는다. 주기를 여러 번
//     놓쳤으면 몰아서 실행하지 않고 한 번만 실행한 뒤 다음 칸으로 건너뛴다.
//   - 일 안에서 Add·Cancel 해도 된다(자기 자신 Cancel 포함). 일 안에서 Add 한 일은 같은 RunDue 에서 시각이 됐으면 같이
//     실행된다.
//   - 스레드 안전하지 않다. 한 스레드(존 스레드나 타이머 스레드)만 쓴다. 일 안에서 RunDue 를 다시 부르면 ASSERT.
namespace Scheduling
{
    using TimerId = Base::StrongId<struct TimerTag, uint64_t>;

    class TimerHeap
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using Duration = Clock::duration;
        using Job = std::move_only_function<void()>;

        // due 에 job 을 실행한다. period 가 0 보다 크면 그 뒤로 period 마다 실행한다. 발급하는 id 는 1부터(0 = 없음).
        TimerId Add(TimePoint due, Job job, Duration period = Duration::zero());

        // 아직 실행 전(주기 일은 다음 실행 전)이면 지우고 true. 이미 끝났거나 없는 id 면 false.
        // 힙 칸은 그대로 두고 표시만 지운다(꺼낼 때 버린다). 지운 칸이 많이 쌓이면 힙을 다시 만든다.
        bool Cancel(TimerId id);

        // now 까지 시각이 된 일을 시각 순서로 실행한다. 실행한 일 수.
        std::size_t RunDue(TimePoint now);

        // 가장 이른 일의 시각(없으면 nullopt). 그때까지 자면 된다. 지운 칸을 치우므로 const 가 아니다.
        [[nodiscard]] std::optional<TimePoint> NextDue();

        // 남은 일 수(지운 것 제외)
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _timers.size();
        }

    private:
        // 일(move_only_function)이 복사되지 않아 Timer 도 옮기기만 한다
        struct Timer
        {
            Timer(const TimePoint dueAt, const Duration every, Job work)
                : due(dueAt), period(every), job(std::move(work))
            {
            }
            ~Timer() = default;
            Timer(const Timer&) = delete;
            Timer& operator=(const Timer&) = delete;
            Timer(Timer&&) noexcept = default;
            Timer& operator=(Timer&&) noexcept = default;

            TimePoint due;
            Duration period;
            Job job;
        };

        // 힙 칸: 시각과 id 만(일은 _timers 에). 같은 시각이면 id(= 넣은 순서)가 작은 것이 먼저.
        struct Slot
        {
            TimePoint due;
            uint64_t id;

            // priority_queue 는 큰 것을 위에 두므로 "나중 것이 작다"로 비교한다
            bool operator<(const Slot& other) const noexcept
            {
                return due != other.due ? due > other.due : id > other.id;
            }
        };

        void PopStale();
        void CompactIfMostlyStale();

        std::priority_queue<Slot> _heap{};
        std::unordered_map<uint64_t, Timer> _timers{};
        uint64_t _nextId{1};
        // RunDue 가 지금 실행 중인 일(일 안에서 자기 자신을 Cancel 하는 경우를 알아채려고)
        uint64_t _runningId{};
        bool _runningPeriodic{};
        bool _runningCanceled{};
    };
} // namespace Scheduling
