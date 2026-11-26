#pragma once

#include <cstdint>

#include "Base/Assert.h"
#include "Base/StrongId.h"

// Ruid(RockQoo Unique ID): 모든 서버에서 유일한 64비트 ID. 플레이어, 길드, 우편, 아이템 등 콘텐츠 ID의 원본이다.
// 발급은 RuidGenerator가 한다. 이 파일은 타입과 비트 배치만 정한다.
//
// 비트 배치 (위부터)
//   bit 63      Sign       늘 0. SQL Server BIGINT(부호 있음)와 C# long에 그대로 들어간다
//   bit 62..22  Timestamp  41비트. 기준 시각(2026-01-01 UTC)부터 지난 ms. 약 69.7년(2095년까지)
//   bit 21..15  Realm      7비트. 플레이어가 고르는 서버 하나("1서버"). 1~127, 0은 예약
//   bit 14..9   Node       6비트. 그 렐름 안에서 ID를 만드는 프로세스 번호. 0~63
//   bit 8..0    Sequence   9비트. 같은 프로세스, 같은 ms 안의 순번. 0~511
//
// 시각이 맨 위라 나중에 만든 ID가 항상 크다. DB 클러스터드 인덱스에 늘 끝으로 붙고(페이지 분할이 적다),
// ID 범위로 기간을 조회할 수 있다.
//
// 주의사항
//   1. 비트 배분과 기준 시각은 첫 ID를 발급한 뒤로 바꾸지 않는다. DB에 남은 ID와 뜻이 어긋난다.
//   2. Realm 번호는 모든 지역에서 하나의 번호표로 매긴다. 지역마다 1부터 매기면 서버 통합 때 ID가 겹친다.
//   3. Node 번호는 같은 렐름에서 동시에 떠 있는 발급 프로세스끼리 달라야 한다. 같으면 같은 ms에 같은 ID가 나온다.
//      순번(CAS)은 프로세스 안의 스레드끼리만 나눠 쓰기 때문이다. 꺼진 프로세스의 번호는 다시 써도 된다(시각이 다르다).
//      ID를 만들지 않는 프로세스(Gateway 등)는 번호가 필요 없다.
//   4. 상한: 렐름 127개, 렐름당 발급 프로세스 64개, 프로세스당 ms마다 512개. 512개를 넘으면 다음 ms까지 기다리므로
//      대량 발급(전체 우편 등)은 나눠서 한다.
//   5. 아래 비트가 Sequence라서 `id % N`으로 나누면 한쪽에 몰린다(드물게 만들면 Sequence가 늘 0이다).
//      Lane 등을 고를 때는 비트를 섞는 해시를 쓴다.
//   6. JavaScript(웹 툴)는 2^53까지만 정확하다. JSON으로 내보낼 때는 문자열로 쓴다.
//   7. ID만 보고 만든 시각, 렐름, 노드를 알 수 있다. 클라이언트에 보이는 ID라면 이 점을 감안한다.
namespace Base
{
    // 다른 ID와 섞이지 않는 정수 ID(StrongId.h). 값을 꺼낼 때(DB 바인딩, 로그)는 id.Value().
    // Ruid{}(0)는 "아직 없음"이다. Realm 0을 예약해서 실제로 발급된 ID는 0이 될 수 없다.
    using Ruid = StrongId<struct RuidTag>;

    inline constexpr int RuidTimestampBits{41};
    inline constexpr int RuidRealmBits{7};
    inline constexpr int RuidNodeBits{6};
    inline constexpr int RuidSequenceBits{9};
    static_assert(RuidTimestampBits + RuidRealmBits + RuidNodeBits + RuidSequenceBits == 63,
                  "부호 비트 하나는 비워 둔다(음수가 나오지 않게)");

    inline constexpr int RuidNodeShift{RuidSequenceBits};
    inline constexpr int RuidRealmShift{RuidNodeShift + RuidNodeBits};
    inline constexpr int RuidTimestampShift{RuidRealmShift + RuidRealmBits};

    inline constexpr std::int64_t RuidMaxTimestamp{(std::int64_t{1} << RuidTimestampBits) - 1};
    inline constexpr std::int32_t RuidMinRealm{1}; // 0은 예약(Ruid{} = 없음, Init을 빼먹은 프로세스 잡기)
    inline constexpr std::int32_t RuidMaxRealm{(1 << RuidRealmBits) - 1};
    inline constexpr std::int32_t RuidMaxNode{(1 << RuidNodeBits) - 1};
    inline constexpr std::int32_t RuidMaxSequence{(1 << RuidSequenceBits) - 1};

    // 기준 시각 2026-01-01T00:00:00Z의 유닉스 ms. 바꾸지 않는다(주의사항 1).
    // 1970년을 그대로 쓰면 41비트 중 56년을 이미 쓴 채로 시작한다.
    inline constexpr std::int64_t RuidEpochUnixMs{1'767'225'600'000};

    // 칸 네 개로 ID를 만든다. 범위를 벗어나면 ASSERT로 멈춘다(잘못된 ID는 DB에 영원히 남아 고칠 수 없다).
    // timestamp는 기준 시각부터 지난 ms다(유닉스 ms가 아니다).
    constexpr Ruid MakeRuid(const std::int64_t timestamp, const std::int32_t realm, const std::int32_t node,
                            const std::int32_t sequence) noexcept
    {
        ASSERT(timestamp >= 0 && timestamp <= RuidMaxTimestamp, "Ruid timestamp out of range");
        ASSERT(realm >= RuidMinRealm && realm <= RuidMaxRealm, "Ruid realm out of range");
        ASSERT(node >= 0 && node <= RuidMaxNode, "Ruid node out of range");
        ASSERT(sequence >= 0 && sequence <= RuidMaxSequence, "Ruid sequence out of range");

        return Ruid{(timestamp << RuidTimestampShift) | (std::int64_t{realm} << RuidRealmShift) |
                    (std::int64_t{node} << RuidNodeShift) | std::int64_t{sequence}};
    }

    // ID에서 칸을 꺼낸다. 로그에 남은 ID를 "언제, 어느 렐름의 어느 노드가 만들었나"로 되짚을 때 쓴다.
    constexpr std::int64_t RuidTimestamp(const Ruid id) noexcept
    {
        return (id.Value() >> RuidTimestampShift) & RuidMaxTimestamp;
    }

    constexpr std::int32_t RuidRealm(const Ruid id) noexcept
    {
        return static_cast<std::int32_t>((id.Value() >> RuidRealmShift) & RuidMaxRealm);
    }

    constexpr std::int32_t RuidNode(const Ruid id) noexcept
    {
        return static_cast<std::int32_t>((id.Value() >> RuidNodeShift) & RuidMaxNode);
    }

    constexpr std::int32_t RuidSequence(const Ruid id) noexcept
    {
        return static_cast<std::int32_t>(id.Value() & RuidMaxSequence);
    }

    // 만든 시각(유닉스 ms)
    constexpr std::int64_t RuidUnixMs(const Ruid id) noexcept
    {
        return RuidTimestamp(id) + RuidEpochUnixMs;
    }
} // namespace Base
