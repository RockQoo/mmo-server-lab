#include "Base/Assert.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Base/Utf8.h"

namespace Base
{
    void AssertFailed(const std::string_view condition, const std::string_view message,
                      const std::source_location& location)
    {
        // 1)~2) 기록. 문자열 만들기·출력이 예외를 던져도(메모리 부족, stderr 쓰기 실패) 밖으로 내보내지 않는다.
        // 밖으로 나가면 아래 크래시에 닿지 못하고, 누군가 그 예외를 잡으면 ASSERT가 깨졌는데도 서버가 계속 돈다.
        try
        {
            const auto text = std::format("[ASSERT] {}\n  조건: {}\n  위치: {}({})\n  함수: {}\n", message, condition,
                                          location.file_name(), location.line(), location.function_name());

            // 1) 콘솔
            std::print(stderr, "{}", text);
            std::fflush(stderr);

            // 2) VS 출력 창. ...A 버전은 시스템 코드 페이지(CP949)로 읽어 한글이 깨지므로 UTF-16으로 바꿔 W로 보낸다.
            auto wide = std::wstring{};
            if (DecodeUtf8(text, wide))
            {
                OutputDebugStringW(wide.c_str());
            }
            else
            {
                OutputDebugStringA(text.c_str()); // 메시지에 깨진 UTF-8이 섞인 경우
            }
        }
        catch (...)
        {
            OutputDebugStringW(L"[ASSERT] 진단 메시지를 남기지 못했다(출력 중 예외)\n"); // 할당 없는 고정 문자열
        }

        // 3) F5로 실행 중이면 여기서 멈춘다. 호출 스택에서 한 칸 위가 ASSERT를 쓴 줄이다.
        if (IsDebuggerPresent())
        {
            __debugbreak();
        }

        // 4) 크래시. 계속 실행할 수 없는 예외로 낸다.
        RaiseException(AssertExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::abort(); // 누군가 예외를 삼켜 돌아오더라도 계속 실행하지 않는다
    }
} // namespace Base
