#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// 늘기만 하는 횟수(받은 패킷 수, 접속 수, 보낸 바이트, DB 저장 실패 수). 어느 스레드에서 Add 해도 된다.
// 읽는 쪽은 Value()를 가끔(예: 10초마다 Scheduler 로) 읽어 이전 값과의 차이로 초당 몇 개인지 본다.
//
//     Metrics::MetricCounter received;
//     received.Add();          // IOCP 스레드 여럿이 동시에
//     received.Add(bytes);
//     const auto total = received.Value();
//
// 칸 나누기: atomic 하나를 여러 스레드가 올리면 같은 캐시 줄을 두고 다툰다(줄이 코어 사이를 오간다). 그래서 캐시 줄마다
// 띄운 칸 StripeCount 개를 두고, 스레드마다 정해진 칸 하나만 올린다. 읽을 때 칸을 모두 더한다.
// 비용: 카운터 하나에 64B × StripeCount(1KB). 카운터는 서버에 수십 개라 괜찮다.
// atomic 하나와 비교한 측정은 Bench/MetricCounterBench.cpp.
//
// Value()는 더하는 동안 다른 스레드가 올린 것이 들어갈 수도 빠질 수도 있다(그 순간의 정확한 값이 아니다). 늘기만 하므로
// 다음에 읽으면 들어간다. 이름이 BCL System.Diagnostics.Metrics.Counter<T> 와 겹치지 않게 MetricCounter 다.
namespace Metrics
{
    class MetricCounter
    {
    public:
        static constexpr std::size_t StripeCount = 16;

        MetricCounter() = default;
        ~MetricCounter() = default;
        // 이름 붙은 자리 하나에 두고 가리켜 쓴다(atomic 이라 복사·이동하지 않는다)
        MetricCounter(const MetricCounter&) = delete;
        MetricCounter& operator=(const MetricCounter&) = delete;
        MetricCounter(MetricCounter&&) = delete;
        MetricCounter& operator=(MetricCounter&&) = delete;

        // delta 만큼 늘린다(0 이상. 줄이는 값은 Gauge 로 따로 둔다)
        void Add(const int64_t delta = 1) noexcept
        {
            // relaxed: 다른 메모리와의 순서는 필요 없다(세기만 한다)
            _stripes[CurrentStripe()].value.fetch_add(delta, std::memory_order_relaxed);
        }

        // 지금까지 늘린 합
        [[nodiscard]] int64_t Value() const noexcept;

    private:
        // 캐시 줄 하나(64B)를 칸 하나가 혼자 쓴다(alignas 는 C4324 경고라 패딩으로, Lane 과 같다)
        struct Stripe
        {
            Stripe() = default;
            ~Stripe() = default;
            Stripe(const Stripe&) = delete;
            Stripe& operator=(const Stripe&) = delete;
            Stripe(Stripe&&) = delete;
            Stripe& operator=(Stripe&&) = delete;

            std::atomic<int64_t> value{};
            std::array<std::byte, 64 - sizeof(std::atomic<int64_t>)> padding{};
        };

        // 이 스레드의 칸 번호. 처음 부를 때 한 번 정하고(AssignStripe), 그 뒤로는 thread_local 하나를 읽는다.
        // 상수로 초기화하는 thread_local 이라 MSVC 가 부를 때마다 초기화 확인을 넣지 않는다.
        static std::size_t CurrentStripe() noexcept
        {
            if (t_stripe == NoStripe) [[unlikely]]
            {
                t_stripe = AssignStripe();
            }
            return t_stripe;
        }
        static std::size_t AssignStripe() noexcept;

        static constexpr std::size_t NoStripe = StripeCount;
        static inline thread_local std::size_t t_stripe = NoStripe;

        std::array<Stripe, StripeCount> _stripes{};
    };
} // namespace Metrics
