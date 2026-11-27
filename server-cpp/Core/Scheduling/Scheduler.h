#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "Messaging/Lane.h"
#include "Scheduling/TimerHeap.h"

// "몇 초 뒤", "몇 초마다" 일을 담당 Lane 에 넣어 주는 타이머 스레드 하나. 일은 타이머 스레드가 아니라 등록할 때 넘긴
// Lane 에서 돈다(그 Lane 의 데이터를 락 없이 만진다). 표준(std::jthread, condition_variable)만 쓴다.
//
//     Scheduler scheduler;
//     scheduler.Every(10s, zoneLane, [] { SaveAll(); });    // 10초 뒤부터 10초마다
//     const auto id = scheduler.After(3s, zoneLane, [] { ... }); // 3초 뒤 한 번
//     scheduler.Cancel(id);
//
// 규칙
//   - 시각 규칙은 TimerHeap 과 같다: 고정 간격, 놓친 칸은 한 번만, 같은 시각은 넣은 순서.
//   - 모든 함수는 어느 스레드에서 불러도 된다(Lane 안의 일에서도).
//   - Cancel 이 true 를 돌려주면 그 뒤로 일이 새로 시작하지 않는다. 시각이 돼 Lane 에 이미 넣었지만 아직 안 돈 것도
//     막는다. 다른 스레드에서 Cancel 할 때 Lane 에서 지금 돌고 있는 한 번은 끝까지 돈다(같은 Lane 안에서 Cancel 하면
//     그런 경우가 없다).
//   - Lane 은 Scheduler 보다 오래 살아야 한다(Scheduler 를 먼저 멈춘다). Lane 이 멈추는 중이면 그 회차는 버린다.
namespace Scheduling
{
    class Scheduler
    {
    public:
        using Clock = TimerHeap::Clock;
        using Duration = TimerHeap::Duration;
        using Job = std::move_only_function<void()>;

        // 타이머 스레드 이름(UTF-8, Base::SetCurrentThreadName). 덤프·VS 스레드 창에 보인다.
        explicit Scheduler(std::string_view name = "Scheduler");
        // Stop()과 같다
        ~Scheduler();

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;
        Scheduler(Scheduler&&) = delete;
        Scheduler& operator=(Scheduler&&) = delete;

        // delay 뒤 한 번 lane 에서 job 을 실행한다. 멈춘 뒤면 등록하지 않고 TimerId{}(0).
        TimerId After(Duration delay, Messaging::Lane& lane, Job job);

        // period 뒤부터 period 마다 lane 에서 job 을 실행한다. period 는 0 보다 커야 한다. 멈춘 뒤면 TimerId{}(0).
        TimerId Every(Duration period, Messaging::Lane& lane, Job job);

        // 아직 시작하지 않은 실행을 막고 true. 한 번짜리가 이미 돌았거나 없는 id 면 false.
        bool Cancel(TimerId id);

        // 타이머 스레드를 끝낸다. 남은 일은 더 돌지 않는다(Scheduler 와 같이 사라진다). 두 번 불러도 된다.
        // 이미 Lane 에 넣은 회차는 Lane 이 처리한다.
        void Stop();

    private:
        // 등록한 일 하나. 타이머 스레드(힙의 일)와 Lane 에 넣은 회차가 같이 들고 있다. Scheduler 를 가리키지 않으므로
        // Lane 에 남은 회차가 Scheduler 보다 늦게 돌아도 안전하다.
        struct Entry
        {
            explicit Entry(Job work, const bool repeat) : job(std::move(work)), periodic(repeat)
            {
            }
            ~Entry() = default;
            // shared_ptr 로만 나눠 든다(atomic 이 있어 복사·이동하지 않는다)
            Entry(const Entry&) = delete;
            Entry& operator=(const Entry&) = delete;
            Entry(Entry&&) = delete;
            Entry& operator=(Entry&&) = delete;

            Job job;
            const bool periodic;
            std::atomic<bool>
                canceled{}; // 한 번짜리는 "이미 돌았음"도 이것으로 표시한다(Cancel 과 실행 중 먼저 온 쪽만)
        };

        TimerId Add(Duration delay, Duration period, Messaging::Lane& lane, Job job);
        void Run();
        void SweepExpiredEntries();

        std::mutex _mutex{};
        // Stop 을 여러 스레드가 같이 불러도 joinable 확인부터 join 까지 하나씩. Run 이 _mutex 를 잡고 돌아서 _mutex 를
        // 잡은 채 join 하면 교착이라 따로 둔다.
        std::mutex _stopMutex{};
        std::condition_variable _wake{}; // 더 이른 일이 들어오거나 멈출 때 깨운다
        TimerHeap _heap{};               // _mutex 가 지킨다
        // id → 일. Cancel 이 Lane 에 이미 넣은 회차까지 막으려고 둔다. 끝난 일은 weak_ptr 이 비므로 가끔 치운다.
        std::unordered_map<uint64_t, std::weak_ptr<Entry>> _entries{}; // _mutex 가 지킨다
        std::size_t _sweepAt{};                                        // _entries 가 이만큼 크면 치운다
        bool _stopping{};                                              // _mutex 가 지킨다
        std::jthread _thread{};                                        // 생성자 본문에서 시작, Stop()에서 join
    };
} // namespace Scheduling
