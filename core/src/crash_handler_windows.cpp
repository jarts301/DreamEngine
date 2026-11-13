// Crash reports on Windows: an unhandled exception filter, plus handlers for abort(),
// std::terminate, pure virtual calls and invalid C runtime parameters. The crashed thread only
// hands the crash to a writer thread started in advance, because after a stack overflow it has
// almost no stack left. See crash_report.h.

#include "crash_report.h"

#if defined(_WIN32)

#include <windows.h>
// After windows.h, which they need.
#include <dbghelp.h>
#include <eh.h>
#include <psapi.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <string_view>

namespace dream::core::crash {
namespace {

// What the crashed thread hands to the writer.
struct Request {
    DWORD thread_id = 0;
    EXCEPTION_POINTERS* exception = nullptr;
    // The crashed thread's registers: the exception's, or captured when there was no exception.
    CONTEXT context{};
    FixedText reason;
};

Request g_request;
HANDLE g_writer = nullptr;
HANDLE g_request_ready = nullptr;
HANDLE g_report_written = nullptr;
volatile LONG g_stopping = 0;

// Put back when uninstalled. While installed, crashes don't reach it: the runtime's own filter
// would hand them to Windows Error Reporting, whose dialog would stop an unattended run.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
_purecall_handler g_previous_purecall = nullptr;
_invalid_parameter_handler g_previous_invalid_parameter = nullptr;
std::terminate_handler g_previous_terminate = nullptr;
void(__cdecl* g_previous_abort)(int) = SIG_DFL;
unsigned int g_previous_abort_behavior = 0;

// How long the crashed thread waits for the report before carrying on to end the program.
constexpr DWORD k_report_timeout_ms = 60'000;

// An exception code for minidumps of crashes that had no exception, such as abort(), so a
// debugger still opens them at the crashed thread.
constexpr DWORD k_no_exception_code = 0xE0000DEA;

void to_file(void* target, const char* data, std::size_t size) noexcept {
    const HANDLE file = *static_cast<HANDLE*>(target);
    while (size > 0) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(size > 0x10000000 ? 0x10000000 : size);
        if (WriteFile(file, data, chunk, &written, nullptr) == 0 || written == 0) {
            return;
        }
        data += written;
        size -= written;
    }
}

// CreateFileW, with the UTF-8 path converted on the stack.
HANDLE create_file(const char* utf8_path) noexcept {
    wchar_t wide[sizeof(g_state.directory) + 64];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide, static_cast<int>(std::size(wide))) ==
        0) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

void describe(FixedText& reason, const EXCEPTION_RECORD& record) noexcept {
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        reason.append("Access violation");
        if (record.NumberParameters >= 2) {
            const ULONG_PTR operation = record.ExceptionInformation[0];
            reason.append(operation == 0   ? ", reading address "
                          : operation == 8 ? ", executing address "
                                           : ", writing address ");
            reason.append_hex(record.ExceptionInformation[1]);
        }
        return;
    case EXCEPTION_STACK_OVERFLOW:
        reason.append("Stack overflow: too much recursion, or too much on the stack");
        return;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        reason.append("Integer division by zero");
        return;
    case EXCEPTION_BREAKPOINT:
        reason.append("Breakpoint, which is also how a failed DREAM_VERIFY or DREAM_ASSERT stops");
        return;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        reason.append("Illegal instruction");
        return;
    case EXCEPTION_PRIV_INSTRUCTION:
        reason.append("Privileged instruction");
        return;
    case EXCEPTION_IN_PAGE_ERROR:
        reason.append("A page of memory couldn't be read in, such as from a lost drive");
        return;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        reason.append("Array bounds exceeded");
        return;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
        reason.append("Floating-point error");
        return;
    case 0xE06D7363:
        reason.append("A C++ exception nobody caught");
        return;
    case 0xC0000374:
        reason.append("Heap corruption");
        return;
    default:
        reason.append("Exception ");
        reason.append_hex(record.ExceptionCode);
        return;
    }
}

void write_process_memory(ReportWriter& writer) noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) != 0) {
        writer.text("  Working set: ");
        writer.number(counters.WorkingSetSize / 1024);
        writer.text(" KB (peak ");
        writer.number(counters.PeakWorkingSetSize / 1024);
        writer.text(" KB)\n  Private bytes: ");
        writer.number(counters.PrivateUsage / 1024);
        writer.text(" KB\n");
    }
}

// Walks the crashed thread's stack from its registers, naming each frame with the program's
// debug information (.pdb files) where there is some.
void write_stack(ReportWriter& writer, const Request& request) noexcept {
    writer.text("\nStack trace of the crashing thread, innermost first:\n");
#if defined(_M_X64)
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, request.thread_id);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    const bool symbols = SymInitialize(process, nullptr, TRUE) != 0;

    CONTEXT context = request.context;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 64; ++depth) {
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == 0 ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        writer.text("  #");
        writer.number(static_cast<std::uint64_t>(depth), 2);
        writer.text(" ");
        writer.hex(address);

        const DWORD64 module_base = SymGetModuleBase64(process, address);
        char module_path[MAX_PATH] = {};
        if (module_base != 0 &&
            GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), module_path,
                               MAX_PATH) != 0) {
            const char* name = module_path;
            for (const char* c = module_path; *c != '\0'; ++c) {
                if (*c == '\\' || *c == '/') {
                    name = c + 1;
                }
            }
            writer.text(" ");
            writer.text(name, MAX_PATH);
        }

        alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + 256] = {};
        auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (symbols && SymFromAddr(process, address, &displacement, symbol) != 0) {
            writer.text("!");
            writer.text(symbol->Name, symbol->NameLen);
            writer.text("+");
            writer.number(displacement);
        } else if (module_base != 0) {
            writer.text("+");
            writer.hex(address - module_base);
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (symbols && SymGetLineFromAddr64(process, address, &line_displacement, &line) != 0) {
            writer.text(" (");
            writer.text(line.FileName, MAX_PATH);
            writer.text(":");
            writer.number(line.LineNumber);
            writer.text(")");
        }
        writer.text("\n");
    }
    if (symbols) {
        SymCleanup(process);
    }
    if (thread != nullptr) {
        CloseHandle(thread);
    }
#else
    writer.text("  (only walked on x64)\n");
#endif
}

void write_minidump(const char* path, Request& request) noexcept {
    const HANDLE file = create_file(path);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    // A crash without an exception, such as abort(), gets one made up, so the debugger opens the
    // dump at the crashed thread and place.
    EXCEPTION_RECORD record{};
    EXCEPTION_POINTERS made_up{};
    EXCEPTION_POINTERS* exception = request.exception;
    if (exception == nullptr) {
        record.ExceptionCode = k_no_exception_code;
#if defined(_M_X64)
        record.ExceptionAddress = reinterpret_cast<void*>(request.context.Rip);
#endif
        made_up.ExceptionRecord = &record;
        made_up.ContextRecord = &request.context;
        exception = &made_up;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = request.thread_id;
    information.ExceptionPointers = exception;
    information.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules |
                                                 MiniDumpWithIndirectlyReferencedMemory);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &information,
                      nullptr, nullptr);
    CloseHandle(file);
}

void write_report(Request& request) noexcept {
    const std::int64_t seconds = std::time(nullptr);
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());

    char path[sizeof(g_state.directory) + 64];
    const std::size_t length = report_stem(path, sizeof(path) - 8, seconds, process);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (length > 0) {
        std::memcpy(path + length, ".txt", 5);
        file = create_file(path);
    }
    HANDLE out = file != INVALID_HANDLE_VALUE ? file : GetStdHandle(STD_ERROR_HANDLE);
    {
        ReportWriter writer(&to_file, &out);
        // The parts that need nothing but memory come first, so they are written even if
        // looking up symbols later fails.
        write_header(writer, request.reason.view(), process, request.thread_id, seconds);
        write_details(writer);
        writer.text("\nMemory:\n");
        write_process_memory(writer);
        write_heap(writer);
        write_recent_log(writer);
        writer.flush();
        write_stack(writer, request);
    }
    bool dumped = false;
    if (length > 0 && g_state.minidump) {
        std::memcpy(path + length, ".dmp", 5);
        write_minidump(path, request);
        dumped = true;
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }

    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    ReportWriter message(&to_file, &console);
    message.text("[FATAL] The program crashed: ");
    message.text(request.reason.view());
    if (file != INVALID_HANDLE_VALUE) {
        message.text("\n[FATAL] Crash report: ");
        message.text(std::string_view(path, length));
        message.text(dumped ? ".txt, with a minidump beside it (.dmp)" : ".txt");
    }
    message.text("\n");
}

DWORD WINAPI writer_loop(void* /*unused*/) {
    while (true) {
        WaitForSingleObject(g_request_ready, INFINITE);
        if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0) {
            return 0;
        }
        write_report(g_request);
        SetEvent(g_report_written);
    }
}

// Called on the crashed thread. `exception` is null for crashes that aren't exceptions.
void report(EXCEPTION_POINTERS* exception) noexcept {
    g_request.thread_id = GetCurrentThreadId();
    g_request.exception = exception;
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        g_request.context = *exception->ContextRecord;
    } else {
        RtlCaptureContext(&g_request.context);
    }
    SetEvent(g_request_ready);
    WaitForSingleObject(g_report_written, k_report_timeout_ms);
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* exception) {
    if (begin_report()) {
        g_request.reason = FixedText{};
        describe(g_request.reason, *exception->ExceptionRecord);
        report(exception);
        end_report();
    }
    // Ends the process, with the exception's code as its exit code. A debugger, when attached,
    // sees the exception before this filter runs.
    return EXCEPTION_EXECUTE_HANDLER;
}

void report_without_exception(std::string_view reason) noexcept {
    if (begin_report()) {
        g_request.reason = FixedText{};
        g_request.reason.append(reason);
        report(nullptr);
        end_report();
    }
}

void __cdecl on_abort(int /*signal*/) {
    report_without_exception("abort() was called");
    // Returning lets abort() end the program.
}

void on_terminate() {
    report_without_exception("std::terminate was called: an exception nobody caught, or one "
                             "thrown from noexcept code");
    std::abort();
}

void __cdecl on_purecall() {
    report_without_exception("A pure virtual function was called, usually on an object being "
                             "constructed or destroyed");
    std::abort();
}

void __cdecl on_invalid_parameter(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                                  const wchar_t* /*file*/, unsigned int /*line*/,
                                  std::uintptr_t /*reserved*/) {
    report_without_exception("A C runtime function was given an invalid parameter");
    std::abort();
}

} // namespace

bool install_platform() noexcept {
    g_stopping = 0;
    g_request_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_report_written = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_request_ready == nullptr || g_report_written == nullptr) {
        return false;
    }
    g_writer = CreateThread(nullptr, 256 * 1024, &writer_loop, nullptr, 0, nullptr);
    if (g_writer == nullptr) {
        CloseHandle(g_request_ready);
        CloseHandle(g_report_written);
        return false;
    }

    // Leaves the installing thread, normally the main one, enough stack after an overflow to
    // hand the crash over.
    ULONG guarantee = 32 * 1024;
    SetThreadStackGuarantee(&guarantee);

    g_previous_filter = SetUnhandledExceptionFilter(&on_exception);
    g_previous_abort = std::signal(SIGABRT, &on_abort);
    // No "abort() has been called" dialog, and no second report through Windows Error Reporting:
    // this report replaces both, and the dialog would stop an unattended run.
    g_previous_abort_behavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // The C runtime's own handler. Built without exceptions, as the engine is, std::set_terminate
    // does nothing and std::terminate calls abort(), which the SIGABRT handler above reports. Code
    // built with exceptions, such as some libraries, still reaches this one.
    g_previous_terminate = ::set_terminate(&on_terminate);
    g_previous_purecall = _set_purecall_handler(&on_purecall);
    g_previous_invalid_parameter = _set_invalid_parameter_handler(&on_invalid_parameter);
    return true;
}

void uninstall_platform() noexcept {
    SetUnhandledExceptionFilter(g_previous_filter);
    std::signal(SIGABRT, g_previous_abort);
    _set_abort_behavior(g_previous_abort_behavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::set_terminate(g_previous_terminate);
    _set_purecall_handler(g_previous_purecall);
    _set_invalid_parameter_handler(g_previous_invalid_parameter);

    InterlockedExchange(&g_stopping, 1);
    SetEvent(g_request_ready);
    WaitForSingleObject(g_writer, INFINITE);
    CloseHandle(g_writer);
    CloseHandle(g_request_ready);
    CloseHandle(g_report_written);
    g_writer = nullptr;
    g_request_ready = nullptr;
    g_report_written = nullptr;
}

} // namespace dream::core::crash

#endif
