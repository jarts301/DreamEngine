// Crash reports on Linux: a signal handler that writes the report with system calls only, on a
// stack of its own. See crash_report.h.

#include "crash_report.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <string_view>
#include <typeinfo>

#include <cxxabi.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#endif

namespace dream::core::crash {
namespace {

constexpr std::array<int, 5> k_signals{SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
std::array<struct sigaction, k_signals.size()> g_previous_actions{};
stack_t g_previous_stack{};
bool g_stack_installed = false;
std::terminate_handler g_previous_terminate = nullptr;

// The handler runs on this stack, so a crash caused by running out of stack is reported too. It
// is set up for the thread that installs the handler, normally the main thread.
alignas(16) char g_alternate_stack[64 * 1024];

void write_all(int file, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        const ssize_t written = ::write(file, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void to_file(void* target, const char* data, std::size_t size) noexcept {
    write_all(*static_cast<int*>(target), data, size);
}

std::string_view describe(int number, int code) noexcept {
    switch (number) {
    case SIGSEGV:
        return code == SEGV_ACCERR
                   ? "Segmentation fault (SIGSEGV): memory accessed in a way it doesn't allow"
                   : "Segmentation fault (SIGSEGV): memory that doesn't exist was accessed";
    case SIGBUS:
        return "Bus error (SIGBUS): memory that can't be accessed, such as a truncated file";
    case SIGFPE:
        return code == FPE_INTDIV ? "Arithmetic error (SIGFPE): integer division by zero"
                                  : "Arithmetic error (SIGFPE)";
    case SIGILL:
        return "Illegal instruction (SIGILL), which is also how a failed DREAM_VERIFY or "
               "DREAM_ASSERT stops";
    case SIGABRT:
        return "abort() was called (SIGABRT)";
    default:
        return "A fatal signal";
    }
}

bool has_address(int number) noexcept {
    return number == SIGSEGV || number == SIGBUS || number == SIGFPE || number == SIGILL;
}

// Copies the memory lines of /proc/self/status, read with plain system calls.
void write_process_memory(ReportWriter& writer) noexcept {
    const int file = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return;
    }
    char buffer[4096];
    const ssize_t size = ::read(file, buffer, sizeof(buffer));
    ::close(file);
    if (size <= 0) {
        return;
    }
    std::string_view rest(buffer, static_cast<std::size_t>(size));
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        for (const std::string_view key : {"VmRSS:", "VmHWM:", "RssAnon:", "VmSwap:"}) {
            if (line.starts_with(key)) {
                writer.text("  ");
                writer.text(line);
                writer.text("\n");
            }
        }
    }
}

void write_report(std::string_view reason) noexcept {
    const std::int64_t seconds = std::time(nullptr);
    const auto process = static_cast<std::uint64_t>(::getpid());
    const auto thread = static_cast<std::uint64_t>(::syscall(SYS_gettid));

    char path[sizeof(g_state.directory) + 64];
    const std::size_t length = report_stem(path, sizeof(path) - 8, seconds, process);
    int file = -1;
    if (length > 0) {
        std::memcpy(path + length, ".txt", 5);
        file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    // Without a file, the report goes to stderr rather than nowhere.
    int out = file >= 0 ? file : STDERR_FILENO;
    {
        ReportWriter writer(&to_file, &out);
        write_header(writer, reason, process, thread, seconds);
        write_details(writer);
        writer.text("\nMemory:\n");
        write_process_memory(writer);
        write_heap(writer);
        write_recent_log(writer);
        writer.text("\nStack trace of the crashing thread, innermost first. Each line names the "
                    "program or library and an offset, which\n`addr2line -f -C -e <program> "
                    "<offset>` turns into a function and line when the program has debug "
                    "information:\n");
        writer.flush();
#if defined(__GLIBC__)
        void* frames[64];
        const int count = ::backtrace(frames, static_cast<int>(std::size(frames)));
        ::backtrace_symbols_fd(frames, count, out);
#else
        writer.text("  (not available with this C library)\n");
#endif
    }
    if (file >= 0) {
        ::close(file);
    }

    int console = STDERR_FILENO;
    ReportWriter message(&to_file, &console);
    message.text("[FATAL] The program crashed: ");
    message.text(reason);
    if (file >= 0) {
        message.text("\n[FATAL] Crash report: ");
        message.text(std::string_view(path, length + 4));
    }
    message.text("\n");
}

std::size_t index_of(int number) noexcept {
    for (std::size_t i = 0; i < k_signals.size(); ++i) {
        if (k_signals[i] == number) {
            return i;
        }
    }
    return 0;
}

// Whether a fault at `address` was most likely the stack running out: it is just past the crashed
// thread's stack pointer.
bool looks_like_stack_overflow([[maybe_unused]] const void* context,
                               [[maybe_unused]] std::uintptr_t address) noexcept {
#if defined(__x86_64__)
    if (context == nullptr) {
        return false;
    }
    const auto stack_pointer = static_cast<std::uintptr_t>(
        static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
    const std::uintptr_t distance =
        address > stack_pointer ? address - stack_pointer : stack_pointer - address;
    return distance < 64 * 1024;
#else
    return false;
#endif
}

void on_signal(int number, siginfo_t* info, void* context) {
    if (begin_report()) {
        FixedText reason;
        reason.append(describe(number, info != nullptr ? info->si_code : 0));
        if (info != nullptr && has_address(number)) {
            const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
            reason.append(", at address ");
            reason.append_hex(address);
            if (number == SIGSEGV && looks_like_stack_overflow(context, address)) {
                reason.append(", just past the stack: most likely a stack overflow, from too much "
                              "recursion or too much on the stack");
            }
        }
        write_report(reason.view());
        end_report();
    }
    // What was there before takes over, and the signal does what it would have done, such as
    // ending the process with a core dump.
    ::sigaction(number, &g_previous_actions[index_of(number)], nullptr);
    ::raise(number);
}

void on_terminate() {
    if (begin_report()) {
        FixedText reason;
        reason.append("std::terminate was called");
        // The type of an exception nobody caught, such as St9bad_alloc when memory ran out.
        if (const std::type_info* const type = abi::__cxa_current_exception_type()) {
            reason.append(": an exception nobody caught, of type ");
            reason.append(type->name());
        } else {
            reason.append(", without an exception, such as by a noexcept function throwing");
        }
        write_report(reason.view());
        end_report();
    }
    // The standard library's handler prints the exception, and ends the program.
    if (g_previous_terminate != nullptr) {
        g_previous_terminate();
    }
    std::abort();
}

} // namespace

bool install_platform() noexcept {
#if defined(__GLIBC__)
    // The first call loads the unwinder, which allocates. Doing it now keeps that out of a crash.
    void* warm_up[1];
    static_cast<void>(::backtrace(warm_up, 1));
#endif
    stack_t stack{};
    stack.ss_sp = g_alternate_stack;
    stack.ss_size = sizeof(g_alternate_stack);
    g_stack_installed = ::sigaltstack(&stack, &g_previous_stack) == 0;

    struct sigaction action {};
    action.sa_sigaction = &on_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (std::size_t i = 0; i < k_signals.size(); ++i) {
        ::sigaction(k_signals[i], &action, &g_previous_actions[i]);
    }
    g_previous_terminate = std::set_terminate(&on_terminate);
    return true;
}

void uninstall_platform() noexcept {
    for (std::size_t i = 0; i < k_signals.size(); ++i) {
        ::sigaction(k_signals[i], &g_previous_actions[i], nullptr);
    }
    std::set_terminate(g_previous_terminate);
    if (g_stack_installed) {
        ::sigaltstack(&g_previous_stack, nullptr);
        g_stack_installed = false;
    }
}

} // namespace dream::core::crash

#endif
