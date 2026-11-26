#include "Messaging/LaneGroup.h"

#include <format>
#include <string>

#include "Base/Assert.h"

namespace Messaging
{
    namespace
    {
        // splitmix64의 마무리 단계. 입력 비트 하나가 바뀌면 출력 비트 절반쯤이 바뀌도록 고른 시프트와 상수다.
        // 공식 그대로 옮겼다(바꾸면 C#과 결과가 달라진다).
        constexpr uint64_t Mix(uint64_t x) noexcept
        {
            x ^= x >> 30;
            x *= 0xBF58'476D'1CE4'E5B9;
            x ^= x >> 27;
            x *= 0x94D0'49BB'1331'11EB;
            x ^= x >> 31;
            return x;
        }
    } // namespace

    LaneGroup::LaneGroup(const std::string_view name, const std::size_t count)
    {
        ASSERT(count > 0, "LaneGroup needs at least one Lane");
        _lanes.reserve(count);
        for (auto i = std::size_t{0}; i < count; ++i)
        {
            _lanes.push_back(std::make_unique<Lane>(std::format("{}-{}", name, i)));
        }
    }

    LaneGroup::~LaneGroup()
    {
        for (const auto& lane : _lanes)
        {
            lane->Stop();
        }
        // 여기서부터 _lanes가 해제된다. 모든 Lane 스레드가 끝나 있어 해제된 Lane을 건드리는 일이 없다.
    }

    Lane& LaneGroup::LaneFor(const uint64_t key) noexcept
    {
        return *_lanes[IndexFor(key, _lanes.size())];
    }

    Lane& LaneGroup::At(const std::size_t index) noexcept
    {
        ASSERT(index < _lanes.size(), "LaneGroup::At index out of range");
        return *_lanes[index];
    }

    std::size_t LaneGroup::Count() const noexcept
    {
        return _lanes.size();
    }

    std::size_t LaneGroup::IndexFor(const uint64_t key, const std::size_t count) noexcept
    {
        ASSERT(count > 0, "LaneGroup::IndexFor count is zero");
        return static_cast<std::size_t>(Mix(key) % count);
    }
} // namespace Messaging
