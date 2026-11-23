#include "Messaging/Lane.h"

#include <utility>

#include "Base/Assert.h"

namespace Messaging
{
    Lane::Lane()
    {
        // 본문에서 시작한다: 모든 멤버가 만들어진 뒤라 Run이 덜 만들어진 객체를 보지 않는다
        // (초기화 목록에서 this를 넘기면 C4355).
        thread_ = std::jthread{[this](const std::stop_token stop) { Run(stop); }};
        // 일은 생성자가 끝난 뒤에만 들어온다(Post의 mutex가 이 쓰기를 일 쪽에 보이게 한다).
        id_ = thread_.get_id();
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

        bool wasEmpty{};
        {
            const std::scoped_lock lock{mutex_};
            // 멈춤 요청 확인과 넣기를 같은 락 안에서 한다. Run이 "비었고 멈춤 요청됨"을 보고 끝난 뒤에는
            // 여기서 반드시 멈춤 요청이 보이므로, 끝난 Lane에 넣고 잃어버리는 일이 없다.
            if (thread_.get_stop_token().stop_requested())
            {
                return false;
            }
            wasEmpty = pending_.empty();
            pending_.push_back(std::move(job));
        }
        // 비어 있지 않았다면 Lane 스레드는 이미 깨어 있거나 깨우는 중이다. 비어 있을 때만 깨운다.
        if (wasEmpty)
        {
            wake_.notify_one();
        }
        return true;
    }

    void Lane::Stop()
    {
        ASSERT(!IsInLane(), "Lane::Stop called from its own thread");
        if (!thread_.joinable())
        {
            return;
        }
        thread_.request_stop(); // wake_ 대기도 깨운다(condition_variable_any + stop_token)
        thread_.join();
    }

    bool Lane::IsInLane() const noexcept
    {
        return std::this_thread::get_id() == id_;
    }

    void Lane::Run(const std::stop_token stop)
    {
        std::vector<Job> running;
        while (true)
        {
            {
                std::unique_lock lock{mutex_};
                // 일이 들어오거나 멈춤 요청이 올 때까지 잔다. 멈춤 요청이 와도 남은 일이 있으면 먼저 처리한다.
                wake_.wait(lock, stop, [this] { return !pending_.empty(); });
                if (pending_.empty())
                {
                    return; // 멈춤 요청 + 남은 일 없음
                }
                running.swap(pending_);
            }

            for (auto& job : running)
            {
                job();
            }
            running.clear(); // 용량은 남겨 다음 교체 때 pending_으로 넘어간다
        }
    }
} // namespace Messaging
