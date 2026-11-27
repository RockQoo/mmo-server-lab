#include "Metrics/MetricCounter.h"

namespace Metrics
{
    namespace
    {
        // 다음 스레드에 줄 칸 번호. 스레드가 생길 때마다 차례로 돌려 같은 칸에 몰리지 않게 한다.
        // 모든 카운터가 같은 번호를 쓴다(스레드 하나 = 칸 하나, 카운터마다 정하지 않는다).
        std::atomic<std::size_t> g_nextStripe{};
    } // namespace

    std::size_t MetricCounter::AssignStripe() noexcept
    {
        return g_nextStripe.fetch_add(1, std::memory_order_relaxed) % StripeCount;
    }

    int64_t MetricCounter::Value() const noexcept
    {
        auto total = int64_t{0};
        for (const auto& stripe : _stripes)
        {
            total += stripe.value.load(std::memory_order_relaxed);
        }
        return total;
    }
} // namespace Metrics
