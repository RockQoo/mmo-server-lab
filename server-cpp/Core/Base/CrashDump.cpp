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
#include <ProcessSnapshot.h>
// clang-format on
#include <intrin.h>

#include "Base/Assert.h"
#include "Base/ThreadName.h"

namespace Base
{
    namespace
    {
        // 처리 안 된 예외가 아닌 이유로 죽을 때의 종료 코드(덤프의 예외 코드로도 남는다). ASSERT 는 0xE000A55E.
        constexpr DWORD TerminateCode{0xE000'7E70};
        constexpr DWORD AbortCode{0xE000'7E71};
        constexpr DWORD PureCallCode{0xE000'7E72};
        constexpr DWORD InvalidParameterCode{0xE000'7E73};

        // 같은 초·같은 이유 덤프 이름 뒤에 붙일 번호의 상한(넘으면 실패)
        constexpr auto MaxSameNameDumps = int32_t{100};

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
                : _directory(options.directory), _crashKind(options.crashKind), _afterCrash(options.afterCrash),
                  _afterCrashTimeout(static_cast<DWORD>(options.afterCrashTimeout.count())),
                  _request(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
                  _done(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
                  _afterCrashDone(CreateEventW(nullptr, FALSE, FALSE, nullptr))
            {
                ASSERT(_request != nullptr && _done != nullptr && _afterCrashDone != nullptr,
                       "CrashDump: CreateEvent failed");
                ASSERT(options.afterCrashTimeout.count() >= 0 && options.afterCrashTimeout.count() < INFINITE,
                       "CrashDump: afterCrashTimeout out of range");
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
            // method 는 exception 이 없을 때(살아 있는 서버)만 쓴다: 크래시는 어차피 끝날 프로세스라 바로 쓴다.
            std::filesystem::path Write(const DumpKind kind, const std::string_view reason,
                                        EXCEPTION_POINTERS* const exception,
                                        const DumpMethod method = DumpMethod::Direct)
            {
                const std::scoped_lock lock{_mutex};
                _kind = kind;
                _method = exception == nullptr ? method : DumpMethod::Direct;
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
                if (exception != nullptr && _afterCrash != nullptr)
                {
                    // 시간이 지나면 콜백이 덜 끝났어도 돌아간다(프로세스가 끝나면서 덤프 스레드도 같이 끝난다)
                    WaitForSingleObject(_afterCrashDone, _afterCrashTimeout);
                }
                return _ok ? _path : std::filesystem::path{};
            }

            // 덤프 스레드 자신이 크래시 났나. 그때는 덤프를 요청하지 않는다: 요청한 쪽이 _mutex 를 쥐고 이 스레드를
            // 기다리고 있을 수 있어 Write 가 영원히 끝나지 않는다(Codex 리뷰 R2)
            [[nodiscard]] bool IsDumpThread() const noexcept
            {
                return _dumpThreadId.load(std::memory_order_relaxed) == GetCurrentThreadId();
            }

        private:
            void Run()
            {
                _dumpThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
                static_cast<void>(SetCurrentThreadName("CrashDump"));
                while (true)
                {
                    WaitForSingleObject(_request, INFINITE);
                    _ok = TryWriteDumpFile();
                    // _done 뒤에는 요청 칸이 다음 요청으로 바뀔 수 있어 먼저 읽는다(크래시면 요청한 쪽이 아직 기다린다)
                    const auto crash = _exception != nullptr;
                    SetEvent(_done);
                    if (crash && _afterCrash != nullptr)
                    {
                        CallAfterCrash();
                        SetEvent(_afterCrashDone);
                    }
                }
            }

            void CallAfterCrash() const noexcept
            {
                try
                {
                    _afterCrash();
                }
                catch (...)
                {
                    static_cast<void>(std::fputs("CrashDump: afterCrash threw\n", stderr));
                }
            }

            // 경로 만들기(std::format, path)는 할당 실패로 던질 수 있다. 던지면 이 스레드가 terminate 되고 요청한 쪽은
            // _done 을 영원히 기다리므로 실패로 바꾼다(Codex 리뷰 R2)
            bool TryWriteDumpFile() noexcept
            {
                try
                {
                    return WriteDumpFile();
                }
                catch (...)
                {
                    return false;
                }
            }

            bool WriteDumpFile()
            {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                const auto stem = std::format(L"{}_{:04}{:02}{:02}_{:02}{:02}{:02}_pid{}_{}", _processName, now.wYear,
                                              now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                              GetCurrentProcessId(), std::wstring_view{_reason.data()});
                // 같은 초에 같은 이유로 또 남기면 덮어쓰지 않고 _2, _3 … 을 붙인다(Codex 리뷰 R4: 연달아 뜬 덤프를
                // 비교하는 일이 있다)
                auto file = INVALID_HANDLE_VALUE;
                for (auto n = int32_t{1}; n <= MaxSameNameDumps && file == INVALID_HANDLE_VALUE; ++n)
                {
                    _path = _directory / (n == 1 ? stem + L".dmp" : std::format(L"{}_{}.dmp", stem, n));
                    file = CreateFileW(_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
                    if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
                    {
                        break;
                    }
                }
                if (file == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
                const auto type = _kind == DumpKind::Mini ? MiniFlags : FullFlags;
                auto ok = false;
                if (_method == DumpMethod::Snapshot)
                {
                    ok = WriteFromSnapshot(file, type);
                }
                else
                {
                    auto info = MINIDUMP_EXCEPTION_INFORMATION{_threadId, _exception, FALSE};
                    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                           _exception != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
                }
                CloseHandle(file);
                return ok;
            }

            // 순간 복사본(copy-on-write)을 떠서 덤프한다. 복사본을 뜨는 동안만 스레드가 멈추고, 파일 쓰기는 복사본에서
            // 하므로 서버는 그동안 계속 돈다(procdump -r 과 같은 방법).
            static bool WriteFromSnapshot(const HANDLE file, const MINIDUMP_TYPE type)
            {
                constexpr auto CaptureFlags =
                    PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION |
                    PSS_CAPTURE_HANDLE_BASIC_INFORMATION | PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION |
                    PSS_CAPTURE_HANDLE_TRACE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT |
                    PSS_CAPTURE_THREAD_CONTEXT_EXTENDED | PSS_CREATE_BREAKAWAY | PSS_CREATE_BREAKAWAY_OPTIONAL |
                    PSS_CREATE_USE_VM_ALLOCATIONS | PSS_CREATE_RELEASE_SECTION;
                HPSS snapshot{};
                if (PssCaptureSnapshot(GetCurrentProcess(), CaptureFlags, CONTEXT_ALL, &snapshot) != ERROR_SUCCESS)
                {
                    return false;
                }
                // MiniDumpWriteDump 에 "이 핸들은 프로세스가 아니라 스냅샷"이라고 알리는 콜백
                auto callback = MINIDUMP_CALLBACK_INFORMATION{
                    [](PVOID, const PMINIDUMP_CALLBACK_INPUT input, const PMINIDUMP_CALLBACK_OUTPUT output) -> BOOL
                    {
                        if (input->CallbackType == IsProcessSnapshotCallback)
                        {
                            output->Status = S_FALSE;
                        }
                        return TRUE;
                    },
                    nullptr};
                const auto ok = MiniDumpWriteDump(static_cast<HANDLE>(snapshot), GetCurrentProcessId(), file, type,
                                                  nullptr, nullptr, &callback) != FALSE;
                static_cast<void>(PssFreeSnapshot(GetCurrentProcess(), snapshot));
                return ok;
            }

            std::filesystem::path _directory;
            DumpKind _crashKind;
            void (*_afterCrash)();
            DWORD _afterCrashTimeout; // ms
            std::wstring _processName{};
            HANDLE _request;
            HANDLE _done;
            HANDLE _afterCrashDone;
            std::atomic<DWORD> _dumpThreadId{};
            std::mutex _mutex{};
            // 요청 칸(_mutex 를 잡은 쪽이 채우고, 덤프 스레드가 읽는다)
            DumpKind _kind{};
            DumpMethod _method{};
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
            if (dumper.IsDumpThread())
            {
                static_cast<void>(std::fputs("CrashDump: dump thread crashed, no dump\n", stderr));
                return;
            }
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
        // 바깥 API 모양: signal 처리기는 void(int) 로 정해져 있다(int32_t 규칙 예외)
        void OnAbortSignal(int) noexcept
        {
            DumpAndExit("abort", AbortCode);
        }

        void OnPureCall() noexcept
        {
            DumpAndExit("purecall", PureCallCode);
        }

        // 바깥 API 모양: CRT 처리기(_invalid_parameter_handler) 모양 그대로(int32_t 규칙 예외)
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

    std::filesystem::path CrashDump::Write(const DumpKind kind, const std::string_view reason, const DumpMethod method)
    {
        auto* const dumper = g_dumper.load(std::memory_order_acquire);
        ASSERT(dumper != nullptr, "CrashDump::Write called before Install");
        return dumper->Write(kind, reason, nullptr, method);
    }
} // namespace Base
