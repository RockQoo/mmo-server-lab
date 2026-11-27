#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Messaging
{
    // 일(함수)을 받아 전용 스레드 하나에서 넣은 순서대로 하나씩 처리한다.
    // 한 Lane에 묶인 데이터(존 하나 등)는 그 Lane 스레드만 만지므로 락 없이 쓴다(스레드 어피니티).
    //
    // 큐: mutex + 두 버퍼 교체. 넣는 쪽은 _pending에 넣고, Lane 스레드는 _pending을 통째로 바꿔 와서 락 밖에서
    // 처리한다. 락은 "넣기"와 "바꾸기" 동안만 잡는다. 두 vector가 용량을 주고받으므로 일정량이 넘으면 더 할당하지
    // 않는다. (lock-free 큐 등과의 성능 비교는 PROGRESS.md "나중에 비교 실험할 것")
    //
    // 일이 예외를 던지면 잡지 않는다. 스레드 밖으로 나가 std::terminate(크래시)가 된다.
    // 반쯤 바뀐 상태로 계속 도는 것보다 죽고 덤프를 남기는 편이 안전하다(ASSERT와 같은 생각).
    class Lane
    {
    public:
        using Job = std::move_only_function<void()>;

        Lane();
        // 스레드 이름을 붙인다(Base::SetCurrentThreadName, UTF-8). VS 스레드 창, 프로파일러, 크래시 덤프에 이 이름으로
        // 보인다. 생성할 때 한 번만 정하고 바꾸지 않는다. 빈 이름이면 붙이지 않는다.
        explicit Lane(const std::string_view name);
        // Stop()과 같다. 이미 넣은 일은 모두 처리한 뒤 스레드를 끝낸다.
        ~Lane();

        Lane(const Lane&) = delete;
        Lane& operator=(const Lane&) = delete;
        Lane(Lane&&) = delete;
        Lane& operator=(Lane&&) = delete;

        // 일을 넣는다. 어느 스레드에서 불러도 된다(Lane 안의 일에서 자기 Lane에 넣어도 된다).
        // 멈추는 중(Lane 안의 일에서 넣는 것도 포함)이거나 빈 함수면 넣지 않고 false. 버려지는 일을 놓치지 않게 결과를
        // 확인하게 한다.
        [[nodiscard]] bool Post(Job job);

        // 새 일을 막고, 이미 넣은 일을 모두 처리한 뒤 스레드가 끝날 때까지 기다린다. 두 번 불러도 된다.
        // 소유자 스레드에서만 부른다. Lane 자기 스레드에서 부르면 자기를 기다리게 되어 ASSERT로 멈춘다.
        void Stop();

        // 지금 이 코드가 이 Lane 스레드에서 도는가. "이 데이터는 이 Lane에서만" 확인(ASSERT)에 쓴다.
        [[nodiscard]] bool IsInLane() const noexcept;

    private:
        void Run();

        // 메모리 배치(Lane 개선 2단계 A): Post 와 Lane 스레드가 매번 만지는 필드를 한데 모으고 앞뒤를 64B 띄운다.
        // Lane 은 LaneGroup 이 하나씩 힙에 만들므로 띄우지 않으면 이웃 객체와 같은 캐시 줄에 놓일 수 있다.
        // (alignas 는 C4324 경고라 패딩으로)
        std::array<std::byte, 64> _paddingBefore{};
        std::mutex _mutex{};
        std::vector<Job> _pending{};     // _mutex가 지킨다
        bool _stopping{};                // _mutex가 지킨다. Stop이 켠다(Post가 보고 넣지 않는다)
        std::condition_variable _wake{}; // 일이 들어오거나 _stopping이 켜지면 깨운다
        std::array<std::byte, 64> _paddingAfter{};
        std::string _name{};    // 스레드 이름(UTF-8). 스레드 시작 전에 정해진다
        std::jthread _thread{}; // 생성자 본문에서 시작, Stop()에서 join
    };
} // namespace Messaging
