#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

// 덤프 파일(.dmp)을 남긴다. 두 가지 쓰임:
// - 자동(크래시): Install() 뒤로는 처리 안 된 예외(접근 위반, ASSERT 등), std::terminate, 순수 가상 함수 호출,
//   CRT 잘못된 인자가 모두 덤프를 남기고 프로세스를 끝낸다.
// - 수동(살아 있는 서버): Write(kind, reason). 서버는 계속 돈다. 멈춤·메모리 문제를 죽이지 않고 조사할 때
//   (나중에 관리 콘솔 명령이 부른다).
//
// 덤프는 미리 만든 덤프 전용 스레드가 쓴다. 크래시 난 스레드는 요청만 하고 기다린다: 스택 오버플로처럼 그 스레드의
// 스택이 거의 없어도 덤프를 쓸 수 있고, 덤프에 그 스레드의 스택이 멈춘 그대로 남는다.
//
// 파일: <directory>/<프로세스 이름>_<날짜>_<시각>_pid<번호>_<reason>.dmp
//   예: ZoneServer_20261127_153012_pid1234_crash.dmp
// 디버거가 붙어 있으면 처리 안 된 예외는 디버거가 먼저 받는다(덤프 대신 그 자리에서 멈춤).
namespace Base
{
    enum class DumpKind : std::uint8_t
    {
        Mini, // 스레드·스택·전역 변수·핸들(수 MB). 크래시 기본
        Full, // 프로세스 메모리 전체(GB 단위일 수 있음)
    };

    // 살아 있는 서버 덤프(Write)의 방법
    enum class DumpMethod : std::uint8_t
    {
        // 프로세스의 순간 복사본(PssCaptureSnapshot, copy-on-write)을 떠서 그것을 쓴다. 서버가 멈추는 것은 복사본을
        // 뜨는 짧은 순간뿐이다(Full 덤프도). 기본
        Snapshot,
        // 프로세스를 바로 쓴다. 덤프를 쓰는 동안(Full 이면 메모리 크기에 비례해 수 초) 모든 스레드가 멈춘다.
        // 비교 측정과 스냅샷이 안 되는 환경용
        Direct,
    };

    struct CrashDumpOptions
    {
        std::filesystem::path directory = "dumps";
        DumpKind crashKind = DumpKind::Mini;
    };

    class CrashDump
    {
    public:
        CrashDump() = delete;

        // 처리기를 등록하고 덤프 전용 스레드를 만든다. 프로세스 시작 때 한 번(두 번 부르면 ASSERT).
        static void Install(const CrashDumpOptions& options);

        // 지금 덤프를 남긴다(서버는 계속 돈다). reason 은 파일 이름에 들어간다(영문·숫자 권장). 남긴 파일 경로, 실패면
        // 빈 경로. 파일을 다 쓸 때까지 부른 스레드는 기다린다(Lane 일 안에서 부르면 그 Lane 이 그동안 멈춘다. 관리
        // 콘솔은 자기 스레드에서 부른다). Install 전에 부르면 ASSERT.
        static std::filesystem::path Write(DumpKind kind, std::string_view reason,
                                           DumpMethod method = DumpMethod::Snapshot);
    };
} // namespace Base
