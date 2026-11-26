#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <new>

#include "Base/Ruid.h"

// Ruid를 발급한다. 여러 스레드가 동시에 Next()를 불러도 같은 값이 나오지 않는다.
//
// 방식: 마지막으로 낸 (시각, 순번)을 원자 변수 하나에 담고 CAS(compare_exchange)로 바꾼다. 바꾸는 데 성공한 스레드만
// 그 값을 가져가므로 같은 (시각, 순번)은 두 번 나오지 않는다.
// 락이 없다(lock-free. wait-free는 아니다: 경쟁하면 재시도한다).
// 같은 ms에 512개를 다 쓰면 다음 ms까지 양보(yield)하며 기다린다.
//
// 시계: 시작할 때 실제 시각을 한 번 읽고, 그 뒤로는 뒤로 가지 않는 steady_clock으로 흐른 시간만 더한다(SteadyClock).
// 그래서 실행 중에 시스템 시계가 뒤로 가도(시간 동기화, VM 복원) ID가 겹치지 않는다.
//
// 쓰는 법: 보통은 Ruid::Init(realm, node) / Ruid::Create()를 쓴다(안에서 아래 전역 발급기를 부른다).
// 인스턴스를 직접 만드는 것은 테스트(가짜 시계, 여러 노드 흉내)용이다.
//
// 주의사항 (Ruid.h 주의사항과 함께 본다)
//   1. 같은 렐름에서 동시에 떠 있는 프로세스끼리 node가 달라야 한다. CAS는 프로세스 안의 스레드끼리만 막아 준다.
//      지금은 실행 인자·Config로 주고, Coordinator 단계부터는 Coordinator가 빌려 준다.
//   2. 재시작 사이에 시스템 시계가 뒤로 가 있으면, 꺼지기 전에 낸 ID와 겹칠 수 있다(단조 시계는 프로세스 안에서만
//      단조다). DB 단계에서 "시작할 때 이 노드가 마지막으로 쓴 시각 이후부터 발급" 검사를 붙인다.
//   3. 오래 켜 두면 ID 안의 시각이 실제 시각과 몇 초 벌어질 수 있다(steady_clock과 시스템 시계의 차이).
//      ID 안의 시각은 "대략 언제"용이고, 순서(나중 ID가 크다)는 그대로 지켜진다.
//   4. 한 프로세스가 ms당 512개를 넘게 만들면 그 프로세스의 모든 스레드가 다음 ms까지 기다린다.
//      대량 발급(전체 우편 등)은 나눠서 한다. 기다린 호출 수는 SequenceWaitCount()로 본다.
namespace Base
{
// _last를 캐시 라인에 혼자 두려고 넣은 alignas 때문에 생기는 패딩 안내(C4324). 일부러 넣은 패딩이다.
#pragma warning(push)
#pragma warning(disable : 4324)
    class RuidGenerator
    {
    public:
        // 기준 시각(Ruid::EpochUnixMs)부터 지난 ms를 돌려주는 시계. 테스트는 가짜 시계를 넣는다.
        using Clock = std::function<int64_t()>;

        // 범위를 벗어난 realm(1~127), node(0~63)는 ASSERT로 멈춘다.
        RuidGenerator(const int32_t realm, const int32_t node);
        RuidGenerator(const int32_t realm, const int32_t node, Clock clock);

        // 원자 변수를 들고 있고, 복사하면 두 발급기가 같은 ID를 낸다.
        RuidGenerator(const RuidGenerator&) = delete;
        RuidGenerator& operator=(const RuidGenerator&) = delete;
        RuidGenerator(RuidGenerator&&) = delete;
        RuidGenerator& operator=(RuidGenerator&&) = delete;

        // 새 ID. 이 발급기가 낸 ID는 부른 순서대로 커진다(한 스레드 안에서는 항상 앞의 것보다 크다).
        // 시계가 범위(기준 시각 ~ 69.7년)를 벗어나면 ASSERT로 멈춘다(잘못된 ID는 DB에 남아 고칠 수 없다).
        Ruid Next();

        int32_t Realm() const noexcept;
        int32_t Node() const noexcept;

        // 같은 ms의 순번 512개를 다 써서 기다린 Next() 호출 수(한 호출이 여러 번 돌아도 1).
        // 0이 아니면 대량 발급을 나눌지 본다.
        int64_t SequenceWaitCount() const noexcept;

        // 기본 시계. 부른 순간 실제 시각을 한 번 읽고, 이후는 steady_clock으로 흐른 시간만 더한다.
        static Clock SteadyClock();

        // 프로세스 전역 발급기. main에서 한 번만 부른다(두 번이면 ASSERT).
        static void InitializeGlobal(const int32_t realm, const int32_t node);
        // InitializeGlobal 전에 부르면 ASSERT(Init을 빼먹은 프로세스가 아무 번호로 발급하지 않게).
        static RuidGenerator& Global();

    private:
        // 자주 읽기만 하는 값. _last와 다른 캐시 라인에 둔다.
        Clock _clock{};
        int32_t _realm{};
        int32_t _node{};
        std::atomic<int64_t> _sequenceWaits{};

        // 마지막으로 낸 (시각 << 9 | 순번). 두 값을 한 변수에 담아야 CAS 한 번으로 같이 바뀐다
        // (따로 두면 둘 사이에 다른 스레드가 끼어든다). -1은 "아직 안 냄"(시각 -1, 그래서 시각 0부터 낼 수 있다).
        // 모든 스레드가 CAS로 쓰는 변수라 캐시 라인에 혼자 둔다(거짓 공유 방지). 그래서 맨 뒤에 둔다.
        alignas(std::hardware_destructive_interference_size) std::atomic<int64_t> _last{-1};
    };
#pragma warning(pop)
} // namespace Base
