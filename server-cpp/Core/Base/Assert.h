#pragma once

#include <source_location>
#include <string_view>

// 조건이 깨지면 서버를 멈춘다. 상태가 오염된 채로 계속 도는 것보다 기록을 남기고 죽는 편이 안전하다.
//
//   ASSERT(조건, 메시지)        Debug·Release 모두 검사한다. 기본은 이것.
//   DEBUG_ASSERT(조건, 메시지)  Debug에서만 검사한다. 프로파일링에서 비싸다고 나온 검사만 내린다.
//
// 실패하면: 기록(stderr, VS 출력 창) → 디버거가 붙어 있으면 그 자리에서 멈춤 → 크래시(RaiseException).
// 크래시는 일반 크래시와 같은 경로라서, 나중에 CrashDump(SetUnhandledExceptionFilter)가 그대로 덤프를 남긴다.
namespace Base
{
    // ASSERT 실패로 낸 예외의 코드. 덤프에서 일반 크래시(0xC0000005 등)와 구분된다.
    inline constexpr uint32_t AssertExceptionCode{0xE000'A55E};

    // 매크로가 부른다. 돌아오지 않는다. location의 기본값은 부른 쪽(ASSERT를 쓴 줄)이다.
    [[noreturn]] void AssertFailed(const std::string_view condition, const std::string_view message,
                                   const std::source_location& location = std::source_location::current());
} // namespace Base

// 조건은 한 번만 평가한다. 깨질 일이 드물다고 컴파일러에 알린다([[unlikely]]).
#define ASSERT(condition, message)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition)) [[unlikely]]                                                                                 \
        {                                                                                                              \
            ::Base::AssertFailed(#condition, (message));                                                               \
        }                                                                                                              \
    } while (false)

#ifdef NDEBUG
// Release: 평가하지 않는다. if constexpr (false) 안은 컴파일(문법 검사)만 되고 코드가 생기지 않는다.
// 조건에만 쓰는 변수의 "안 쓰임" 경고도 막는다. (sizeof(!(조건)) 은 /analyze 가 C6334 로 막아 이 방식으로 바꿨다)
#define DEBUG_ASSERT(condition, message)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if constexpr (false)                                                                                           \
        {                                                                                                              \
            static_cast<void>(condition);                                                                              \
        }                                                                                                              \
    } while (false)
#else
#define DEBUG_ASSERT(condition, message) ASSERT(condition, message)
#endif
