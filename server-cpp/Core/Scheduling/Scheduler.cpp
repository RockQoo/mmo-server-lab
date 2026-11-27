#include "Scheduling/Scheduler.h"

#include <algorithm>
#include <string>
#include <utility>

#include "Base/Assert.h"
#include "Base/ThreadName.h"
#include "Base/Utf8.h"

namespace Scheduling
{
    namespace
    {
        // _entries 를 처음 치우는 크기. 그 뒤로는 치운 뒤 크기의 2배에서 다시 치운다(나눠 보면 등록 하나에 O(1)).
        constexpr std::size_t MinSweepSize = 64;
    } // namespace

    Scheduler::Scheduler(const std::string_view name) : _sweepAt(MinSweepSize)
    {
        ASSERT(Base::IsValidUtf8(name), "Scheduler name is not valid UTF-8");
        // 본문에서 시작한다: 모든 멤버가 만들어진 뒤라 Run 이 덜 만들어진 객체를 보지 않는다.
        // 이름은 복사해 넘긴다(name 이 가리키는 문자열은 생성자가 끝나면 없을 수 있다). 실패해도 동작에는 영향이 없다.
        _thread = std::jthread{[this, threadName = std::string{name}]
                               {
                                   static_cast<void>(Base::SetCurrentThreadName(threadName));
                                   Run();
                               }};
    }

    Scheduler::~Scheduler()
    {
        Stop();
    }

    TimerId Scheduler::After(const Duration delay, Messaging::Lane& lane, Job job)
    {
        return Add(delay, Duration::zero(), lane, std::move(job));
    }

    TimerId Scheduler::Every(const Duration period, Messaging::Lane& lane, Job job)
    {
        ASSERT(period > Duration::zero(), "Scheduler::Every: period must be positive");
        return Add(period, period, lane, std::move(job));
    }

    TimerId Scheduler::Add(const Duration delay, const Duration period, Messaging::Lane& lane, Job job)
    {
        ASSERT(job != nullptr, "Scheduler: empty job");
        auto entry = std::make_shared<Entry>(std::move(job), period > Duration::zero());
        // 힙의 일(타이머 스레드에서, _mutex 안에서 돈다)은 Lane 에 회차 하나를 넣기만 한다.
        // 락 순서는 Scheduler → Lane 하나뿐이다(Lane 은 일을 자기 락 밖에서 돌리고, 회차는 Scheduler 를 모른다).
        auto post = [lanePtr = &lane, entry]
        {
            // Lane 이 멈추는 중이면 false: 이 회차는 버린다(종료 중)
            static_cast<void>(lanePtr->Post(
                [entry]
                {
                    // 주기 일은 지워졌는지만 본다. 한 번짜리는 "돌았음"을 표시해 Cancel 과 먼저 온 쪽만 이긴다.
                    const auto skip = entry->periodic ? entry->canceled.load() : entry->canceled.exchange(true);
                    if (!skip)
                    {
                        entry->job();
                    }
                }));
        };

        const auto due = Clock::now() + delay;
        std::scoped_lock lock{_mutex};
        if (_stopping)
        {
            return TimerId{};
        }
        const auto before = _heap.NextDue();
        const auto id = _heap.Add(due, std::move(post), period);
        _entries.emplace(id.Value(), entry);
        SweepExpiredEntries();
        if (!before || due < *before)
        {
            _wake.notify_one(); // 지금 자는 시각보다 이르다: 깨워서 다시 잰다
        }
        return id;
    }

    bool Scheduler::Cancel(const TimerId id)
    {
        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock{_mutex};
            const auto found = _entries.find(id.Value());
            if (found == _entries.end())
            {
                return false;
            }
            // 힙에서 지우기 전에 잡는다: 힙의 일이 사라지면 아직 안 돈 한 번짜리 Entry 도 같이 사라지기 때문
            entry = found->second.lock();
            _entries.erase(found);
            static_cast<void>(_heap.Cancel(id)); // 다음 회차를 막는다(이미 Lane 에 넣은 회차는 아래 표시로)
        }
        // 비었으면 이미 끝난 한 번짜리. 아니면 먼저 표시한 쪽이 이긴다(한 번짜리가 막 돌기 시작했으면 false).
        return entry != nullptr && !entry->canceled.exchange(true);
    }

    void Scheduler::Stop()
    {
        if (!_thread.joinable())
        {
            return;
        }
        {
            std::scoped_lock lock{_mutex};
            _stopping = true;
        }
        _wake.notify_one();
        _thread.join();
        // 남은 일은 더 돌지 않고 Scheduler 와 같이 사라진다. 여기서 비우지 않는 이유: 새 힙을 만드는 할당이 예외를 던질
        // 수 있어 소멸자(Stop)가 noexcept 를 못 지킨다. Lane 에 이미 넣은 회차는 Entry 를 따로 들고 있어 상관없다.
    }

    void Scheduler::Run()
    {
        std::unique_lock lock{_mutex};
        while (!_stopping)
        {
            _heap.RunDue(Clock::now());
            const auto next = _heap.NextDue();
            if (next)
            {
                _wake.wait_until(lock, *next); // 시각이 되거나, 더 이른 일이 들어오거나, 멈출 때 깬다
            }
            else
            {
                _wake.wait(lock);
            }
        }
    }

    void Scheduler::SweepExpiredEntries()
    {
        if (_entries.size() < _sweepAt)
        {
            return;
        }
        std::erase_if(_entries, [](const auto& item) { return item.second.expired(); });
        _sweepAt = std::max(MinSweepSize, 2 * _entries.size());
    }
} // namespace Scheduling
