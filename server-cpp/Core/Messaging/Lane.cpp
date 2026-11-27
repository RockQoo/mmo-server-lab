#include "Messaging/Lane.h"

#include <utility>

#include "Base/Assert.h"
#include "Base/ThreadName.h"
#include "Base/Utf8.h"

namespace Messaging
{
    namespace
    {
        // 이 스레드를 돌리는 Lane. 스레드 ID로 비교하지 않는 이유: 끝난 스레드의 ID는 OS가 다시 쓰므로,
        // 멈춘 Lane A와 새 Lane B가 같은 ID를 받으면 B의 일에서 A.IsInLane()이 true가 된다.
        thread_local const Lane* t_currentLane{};
    } // namespace

    Lane::Lane() : Lane(std::string_view{})
    {
    }

    Lane::Lane(const std::string_view name)
    {
        // 이름은 스레드보다 먼저 정한다(Run이 시작하자마자 읽는다). 깨진 UTF-8은 만드는 쪽에서 바로 잡는다.
        ASSERT(Base::IsValidUtf8(name), "Lane name is not valid UTF-8");
        _name = name;
        // 본문에서 시작한다: 모든 멤버가 만들어진 뒤라 Run이 덜 만들어진 객체를 보지 않는다
        // (초기화 목록에서 this를 넘기면 C4355).
        _thread = std::jthread{[this] { Run(); }};
    }

    Lane::~Lane()
    {
        Stop();
    }

    bool Lane::Post(Job job)
    {
        if (!job)
        {
            return false;
        }

        auto wasEmpty = false;
        {
            const std::scoped_lock lock{_mutex};
            // 멈춤 확인과 넣기를 같은 락 안에서 한다. Stop이 같은 락 안에서 _stopping을 켜므로, Run이 "비었고
            // 멈춤 요청됨"을 보고 끝난 뒤에는 여기서 반드시 _stopping이 보여 끝난 Lane에 넣고 잃어버리는 일이 없다.
            // _thread.get_stop_token()을 쓰지 않는 이유: 부를 때마다 멈춤 상태의 참조 카운트를 원자 연산으로
            // 올렸다 내려(stop_token 복사·소멸), 락을 잡은 시간이 길어진다(Lane 개선 1단계, performance.html 사례 8).
            if (_stopping)
            {
                return false;
            }
            wasEmpty = _pending.empty();
            _pending.push_back(std::move(job));
        }
        // 비어 있지 않았다면 Lane 스레드는 이미 깨어 있거나 깨우는 중이다. 비어 있을 때만 깨운다.
        if (wasEmpty)
        {
            _wake.notify_one();
        }
        return true;
    }

    void Lane::Stop()
    {
        ASSERT(!IsInLane(), "Lane::Stop called from its own thread");
        // 동시에 부르면 뒤에 온 쪽은 앞쪽 join 이 끝난 뒤 joinable 이 false 라 바로 돌아간다(Scheduler::Stop 과 같다).
        // _mutex 를 쓰지 않는 이유: join 하는 동안 잡고 있으면 Run 이 일을 꺼내지 못해 교착한다.
        const std::scoped_lock stopLock{_stopMutex};
        if (!_thread.joinable())
        {
            return;
        }
        {
            const std::scoped_lock lock{_mutex};
            _stopping = true; // 이 뒤의 Post는 넣지 않고 false
        }
        _wake.notify_one(); // 자고 있는 Run을 깨워 _stopping을 보게 한다
        _thread.join();
    }

    bool Lane::IsInLane() const noexcept
    {
        return t_currentLane == this;
    }

    void Lane::Run()
    {
        t_currentLane = this; // 이 스레드는 끝날 때까지 이 Lane만 돌린다
        // 실패해도 동작에는 영향이 없다(디버깅 표시용). 결과는 무시한다. 빈 이름이면 붙이지 않는다.
        static_cast<void>(Base::SetCurrentThreadName(_name));
        auto running = std::vector<Job>{};
        while (true)
        {
            {
                std::unique_lock lock{_mutex};
                // 일이 들어오거나 멈춤 요청이 올 때까지 잔다. 멈춤 요청이 와도 남은 일이 있으면 먼저 처리한다.
                // std::condition_variable(일반판)을 쓴다. condition_variable_any + stop_token 은 대기·깨우기마다 내부
                // mutex 와 stop 콜백 등록을 더 거쳐 무겁다(Lane 개선 1단계, performance.html 사례 8).
                _wake.wait(lock, [this] { return !_pending.empty() || _stopping; });
                if (_pending.empty())
                {
                    return; // 멈춤 요청 + 남은 일 없음
                }
                running.swap(_pending);
            }

            for (auto& job : running)
            {
                job();
            }
            running.clear(); // 용량은 남겨 다음 교체 때 _pending으로 넘어간다
        }
    }
} // namespace Messaging
