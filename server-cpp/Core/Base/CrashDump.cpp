#include "Base/CrashDump.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

// windows.h 의 min/max 매크로와 잘 안 쓰는 부분은 끈다. DbgHelp.h 는 windows.h 뒤에 와야 한다.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format off
#include <DbgHelp.h>
// clang-format on
#include <intrin.h>

#include "Base/Assert.h"

namespace Base
{
    namespace
    {
        // 처리 안 된 예외가 아닌 이유로 죽을 때의 종료 코드(덤프의 예외 코드로도 남는다). ASSERT 는 0xE000A55E.
        constexpr DWORD TerminateCode{0xE000'7E70};
        constexpr DWORD AbortCode{0xE000'7E71};
        constexpr DWORD PureCallCode{0xE000'7E72};
        constexpr DWORD InvalidParameterCode{0xE000'7E73};

        // 스레드·스택·전역 변수(DataSegs)·핸들·스택이 가리키는 메모리 조금. 보통 수 MB.
        constexpr auto MiniFlags =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                       MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
        constexpr auto FullFlags =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

        // 덤프 전용 스레드와 요청 한 칸. 요청하는 쪽(크래시 난 스레드 포함)은 칸을 채우고 신호를 보낸 뒤 끝날 때까지
        // 기다린다. 이벤트(SetEvent/Wait)가 칸의 쓰기를 상대 스레드에 보이게 한다. 요청은 mutex 로 하나씩.
        class Dumper
        {
        public:
            explicit Dumper(const CrashDumpOptions& options)
                : _directory(options.directory), _crashKind(options.crashKind),
                  _request(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
                  _done(CreateEventW(nullptr, FALSE, FALSE, nullptr))
            {
                ASSERT(_request != nullptr && _done != nullptr, "CrashDump: CreateEvent failed");
                std::array<wchar_t, MAX_PATH> module{};
                const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
                _processName = std::filesystem::path{std::wstring_view{module.data(), length}}.stem().wstring();
                std::error_code error;
                std::filesystem::create_directories(_directory, error); // 크래시 때 만들지 않게 미리
                // 프로세스가 끝날 때까지 기다리기만 하므로 join 하지 않는다(종료 때 OS 가 거둔다)
                std::thread{[this] { Run(); }}.detach();
            }
            ~Dumper() = delete; // 프로세스 끝까지 산다(크래시가 언제 올지 모른다)
            Dumper(const Dumper&) = delete;
            Dumper& operator=(const Dumper&) = delete;
            Dumper(Dumper&&) = delete;
            Dumper& operator=(Dumper&&) = delete;

            [[nodiscard]] DumpKind CrashKind() const noexcept
            {
                return _crashKind;
            }

            // 덤프 전용 스레드에 쓰게 하고 끝날 때까지 기다린다. exception 이 있으면 덤프가 그 지점을 가리킨다.
            std::filesystem::path Write(const DumpKind kind, const std::string_view reason,
                                        EXCEPTION_POINTERS* const exception)
            {
                const std::scoped_lock lock{_mutex};
                _kind = kind;
                _exception = exception;
                _threadId = GetCurrentThreadId();
                // 파일 이름에 쓸 수 있는 영문·숫자·-만 남긴다(나머지는 _). 고정 크기라 크래시 중에 할당하지 않는다
                _reason.fill(L'\0');
                for (std::size_t i = 0; i < reason.size() && i + 1 < _reason.size(); ++i)
                {
                    const auto c = reason[i];
                    const auto safe =
                        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
                    _reason[i] = safe ? static_cast<wchar_t>(c) : L'_';
                }
                SetEvent(_request);
                WaitForSingleObject(_done, INFINITE);
                return _ok ? _path : std::filesystem::path{};
            }

        private:
            void Run()
            {
                static_cast<void>(SetThreadDescription(GetCurrentThread(), L"CrashDump"));
                while (true)
                {
                    WaitForSingleObject(_request, INFINITE);
                    _ok = WriteDumpFile();
                    SetEvent(_done);
                }
            }

            bool WriteDumpFile()
            {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                _path = _directory / std::format(L"{}_{:04}{:02}{:02}_{:02}{:02}{:02}_pid{}_{}.dmp", _processName,
                                                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                                 GetCurrentProcessId(), std::wstring_view{_reason.data()});
                const auto file = CreateFileW(_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
                auto info = MINIDUMP_EXCEPTION_INFORMATION{_threadId, _exception, FALSE};
                const auto ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                                  _kind == DumpKind::Mini ? MiniFlags : FullFlags,
                                                  _exception != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
                CloseHandle(file);
                return ok;
            }

            std::filesystem::path _directory;
            DumpKind _crashKind;
            std::wstring _processName{};
            HANDLE _request;
            HANDLE _done;
            std::mutex _mutex{};
            // 요청 칸(_mutex 를 잡은 쪽이 채우고, 덤프 스레드가 읽는다)
            DumpKind _kind{};
            EXCEPTION_POINTERS* _exception{};
            DWORD _threadId{};
            std::array<wchar_t, 48> _reason{};
            // 결과 칸(덤프 스레드가 채우고, 요청한 쪽이 읽는다)
            std::filesystem::path _path{};
            bool _ok{};
        };

        std::atomic<Dumper*> g_dumper{};

        // 크래시 덤프. 덤프 중 예외(할당 실패 등)가 나도 처리기 밖으로 내보내지 않는다: 덤프는 못 남겨도 프로세스는
        // 원래 종료 코드로 끝나야 한다.
        void WriteCrashDump(Dumper& dumper, const std::string_view reason, EXCEPTION_POINTERS* const exception) noexcept
        {
            try
            {
                static_cast<void>(dumper.Write(dumper.CrashKind(), reason, exception));
            }
            catch (...)
            {
                static_cast<void>(std::fputs("CrashDump: failed to write dump\n", stderr));
            }
        }

        // 처리기는 모두 noexcept: C 함수(SetUnhandledExceptionFilter 등)에 넘기므로 예외가 나가면 안 된다(C5039).
        // 안에서 예외가 나면(덤프 중 할당 실패 등) 그 자리에서 terminate 된다. 예외가 아닌 이유(terminate 등)로 죽을
        // 때: 지금 위치로 예외 정보를 만들어 덤프에 남기고 끝낸다.
        [[noreturn]] void DumpAndExit(const std::string_view reason, const DWORD code) noexcept
        {
            CONTEXT context{};
            RtlCaptureContext(&context);
            EXCEPTION_RECORD record{};
            record.ExceptionCode = code;
            record.ExceptionAddress = _ReturnAddress();
            EXCEPTION_POINTERS pointers{&record, &context};
            if (auto* const dumper = g_dumper.load(std::memory_order_acquire))
            {
                WriteCrashDump(*dumper, reason, &pointers);
            }
            TerminateProcess(GetCurrentProcess(), code);
            __fastfail(FAST_FAIL_FATAL_APP_EXIT); // TerminateProcess 는 돌아오지 않는다
        }

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* const exception) noexcept
        {
            if (auto* const dumper = g_dumper.load(std::memory_order_acquire))
            {
                WriteCrashDump(*dumper, "crash", exception);
            }
            return EXCEPTION_EXECUTE_HANDLER; // 프로세스를 끝낸다(종료 코드 = 예외 코드)
        }

        void OnTerminate() noexcept
        {
            DumpAndExit("terminate", TerminateCode);
        }

        // std::terminate 처리기는 MSVC 에서 스레드마다 따로라 Install 한 스레드만 덮는다. 다른 스레드(Lane 등)의 기본
        // terminate 는 abort 를 부르고, SIGABRT 처리기는 프로세스 전체라 여기서 잡는다.
        void OnAbortSignal(int) noexcept
        {
            DumpAndExit("abort", AbortCode);
        }

        void OnPureCall() noexcept
        {
            DumpAndExit("purecall", PureCallCode);
        }

        void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) noexcept
        {
            DumpAndExit("invalidparameter", InvalidParameterCode);
        }
    } // namespace

    void CrashDump::Install(const CrashDumpOptions& options)
    {
        ASSERT(g_dumper.load(std::memory_order_acquire) == nullptr, "CrashDump::Install called twice");
        g_dumper.store(new Dumper(options), std::memory_order_release); // 지우지 않는다(위 Dumper 소멸자)

        // Windows 오류 보고 창과 abort 메시지 창을 띄우지 않는다(서버는 창을 기다리면 안 된다)
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        static_cast<void>(_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT));

        SetUnhandledExceptionFilter(OnUnhandledException);
        static_cast<void>(std::set_terminate(OnTerminate));
        static_cast<void>(std::signal(SIGABRT, OnAbortSignal));
        static_cast<void>(_set_purecall_handler(OnPureCall));
        static_cast<void>(_set_invalid_parameter_handler(OnInvalidParameter));
    }

    std::filesystem::path CrashDump::Write(const DumpKind kind, const std::string_view reason)
    {
        auto* const dumper = g_dumper.load(std::memory_order_acquire);
        ASSERT(dumper != nullptr, "CrashDump::Write called before Install");
        return dumper->Write(kind, reason, nullptr);
    }
} // namespace Base
