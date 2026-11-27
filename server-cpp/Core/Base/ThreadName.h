#pragma once

#include <string_view>

// 지금 스레드에 이름을 붙인다. 표준에 없는 기능이라 OS 함수를 쓰고, 그 코드는 ThreadName.cpp 한 곳에만 둔다
// (Windows: SetThreadDescription. 나중에 Linux 로 옮기면 pthread_setname_np 로 이 파일만 바꾼다).
//
// 이름은 크래시 덤프(WinDbg `~` 스레드 목록), VS 스레드 창, 프로파일러에 보인다. 데드락 덤프에서 스레드 수십 개 중
// "Zone-3 이 Log 를 기다린다"를 바로 읽으려고 붙인다(2026-11-28: MiniDumpNormal 덤프에 이름이 남는 것을 확인).
namespace Base
{
    // name 은 UTF-8. 붙였으면 true. 빈 이름은 붙이지 않고 true. 깨진 UTF-8 이거나 OS 가 실패하면 false
    // (디버깅 표시용이라 실패해도 동작에는 영향이 없다).
    bool SetCurrentThreadName(std::string_view name);
} // namespace Base
