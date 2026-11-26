#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "Messaging/Lane.h"

namespace Messaging
{
    // Lane 여러 개를 묶고, 키로 Lane 하나를 고른다. 같은 키는 항상 같은 Lane으로 가므로
    // "같은 키의 일은 넣은 순서대로 하나씩"이 지켜진다(다른 키끼리는 다른 Lane에서 동시에 돈다).
    //
    // 고르는 방법 두 가지
    //   LaneFor(key)  많고 모양을 모르는 키(PlayerId, GuildId 등). 키를 섞은(splitmix64) 뒤 Lane 수로 나눈다.
    //   At(index)     적고 오래 사는 대상(존). 만들 때 번호를 정해 Lane&를 받아 두고 계속 쓴다.
    //
    // 왜 key % N이 아니라 섞어서 나누나: Ruid처럼 아래 비트가 순번인 ID는 드물게 만들면 순번이 늘 0이라
    // key % 8이 늘 0이다(전부 Lane 0). 섞으면 위쪽 비트(시각 등)까지 반영돼 고르게 퍼진다. 비용은 곱셈 두 번.
    // 섞기는 splitmix64 마무리 단계(널리 쓰이는 공식)다. 실행할 때마다, 컴파일러마다, C++과 C#에서 같은 결과가 나온다
    // (std::hash는 컴파일러마다 다르고, C#의 HashCode는 실행마다 다르다).
    //
    // 순서는 같은 키 안에서만 지켜진다. 키 A의 일과 키 B의 일 사이의 순서는 보장하지 않는다.
    class LaneGroup
    {
    public:
        // Lane을 count개 만든다. 스레드 이름은 "name-0" ~ "name-(count-1)"(생성할 때 한 번 정하고 바꾸지 않는다).
        // count가 0이면 ASSERT. 개수는 벤치마크로 정하고, 나중에 Config에서 받는다.
        LaneGroup(const std::string_view name, const std::size_t count);

        // 모든 Lane을 멈춘다. 각 Lane은 남은 일을 처리한 뒤 끝난다.
        ~LaneGroup() = default;

        LaneGroup(const LaneGroup&) = delete;
        LaneGroup& operator=(const LaneGroup&) = delete;
        LaneGroup(LaneGroup&&) = delete;
        LaneGroup& operator=(LaneGroup&&) = delete;

        // 키로 Lane을 고른다. 같은 키는 항상 같은 Lane.
        [[nodiscard]] Lane& LaneFor(const uint64_t key) noexcept;

        // StrongId 계열(Ruid, PlayerId 등)은 정수 값으로 고른다.
        template <typename Id>
            requires requires(const Id& id) { id.Value(); }
        [[nodiscard]] Lane& LaneFor(const Id& id) noexcept
        {
            return LaneFor(static_cast<uint64_t>(id.Value()));
        }

        // 번호로 Lane을 고른다(0 ~ Count()-1). 범위를 벗어나면 ASSERT.
        [[nodiscard]] Lane& At(const std::size_t index) noexcept;

        [[nodiscard]] std::size_t Count() const noexcept;

        // 키가 몇 번 Lane으로 가는지(LaneFor가 쓰는 계산). 분산 테스트, C#과 같은 결과인지 확인에 쓴다.
        [[nodiscard]] static std::size_t IndexFor(const uint64_t key, const std::size_t count) noexcept;

    private:
        // Lane은 스레드가 this를 쥐고 있어 옮길 수 없다. 그래서 포인터로 들고 있는다.
        std::vector<std::unique_ptr<Lane>> _lanes{};
    };
} // namespace Messaging
