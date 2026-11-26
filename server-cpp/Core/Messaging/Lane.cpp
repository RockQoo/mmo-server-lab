#include "Messaging/Lane.h"

#include <utility>

// SetThreadDescription(스레드 이름). windows.h 의 min/max 매크로와 잘 안 쓰는 부분은 끈다.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Base/Assert.h"
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
        // 이름은 스레드보다 먼저 정한다(Run이 시작하자마자 읽는다).
        if (!name.empty())
        {
            const auto ok = Base::DecodeUtf8(name, _name);
            ASSERT(ok, "Lane name is not valid UTF-8");
        }
        // 본문에서 시작한다: 모든 멤버가 만들어진 뒤라 Run이 덜 만들어진 객체를 보지 않는다
        // (초기화 목록에서 this를 넘기면 C4355).
        _thread = std::jthread{[this](const std::stop_token stop) { Run(stop); }};
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
            // 멈춤 요청 확인과 넣기를 같은 락 안에서 한다. Run이 "비었고 멈춤 요청됨"을 보고 끝난 뒤에는
            // 여기서 반드시 멈춤 요청이 보이므로, 끝난 Lane에 넣고 잃어버리는 일이 없다.
            if (_thread.get_stop_token().stop_requested())
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
        if (!_thread.joinable())
        {
            return;
        }
        _thread.request_stop(); // _wake 대기도 깨운다(condition_variable_any + stop_token)
        _thread.join();
    }

    bool Lane::IsInLane() const noexcept
    {
        return t_currentLane == this;
    }

    void Lane::Run(const std::stop_token stop)
    {
        t_currentLane = this; // 이 스레드는 끝날 때까지 이 Lane만 돌린다
        if (!_name.empty())
        {
            // 실패해도 동작에는 영향이 없다(디버깅 표시용). 결과는 무시한다.
            static_cast<void>(SetThreadDescription(GetCurrentThread(), _name.c_str()));
        }
        auto running = std::vector<Job>{};
        while (true)
        {
            {
                std::unique_lock lock{_mutex};
                // 일이 들어오거나 멈춤 요청이 올 때까지 잔다. 멈춤 요청이 와도 남은 일이 있으면 먼저 처리한다.
                _wake.wait(lock, stop, [this] { return !_pending.empty(); });
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
