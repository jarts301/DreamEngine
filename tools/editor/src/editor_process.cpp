#include "editor_process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <string>

namespace dream::editor {

#ifdef _WIN32

struct Process::Handle {
    HANDLE process = nullptr;
};

Process::Process() = default;

namespace {

// One argument as Windows' command line rules read it back: quoted, with the backslashes before a
// quote doubled.
std::wstring quoted(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
        } else {
            result.append(backslashes, L'\\');
        }
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, L'\\');
    return result + L"\"";
}

std::wstring command_line(const std::filesystem::path& program,
                          const std::vector<std::string>& arguments) {
    std::wstring line = quoted(program.wstring());
    for (const std::string& argument : arguments) {
        line += L' ';
        line += quoted(std::filesystem::path(argument).wstring());
    }
    return line;
}

} // namespace

Process::~Process() {
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
    if (m_handle != nullptr && m_handle->process != nullptr) {
        CloseHandle(m_handle->process);
    }
}

bool Process::run(const std::filesystem::path& program, const std::vector<std::string>& arguments,
                  const std::filesystem::path& folder, const std::filesystem::path& log) {
    if (running()) {
        return false;
    }
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                .lpSecurityDescriptor = nullptr,
                                .bInheritHandle = TRUE};
    const HANDLE output = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    std::wstring line = command_line(program, arguments);
    const BOOL created =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       folder.wstring().c_str(), &startup, &info);
    CloseHandle(output);
    if (created == FALSE) {
        return false;
    }
    CloseHandle(info.hThread);
    m_exit_code = -1;
    m_waiting = true;
    m_waiter = std::thread([this, process = info.hProcess] {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        m_exit_code = static_cast<int>(code);
        m_waiting = false;
    });
    return true;
}

bool Process::start(const std::filesystem::path& program, const std::filesystem::path& folder,
                    const std::filesystem::path& log) {
    if (running()) {
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    HANDLE output = INVALID_HANDLE_VALUE;
    if (!log.empty()) {
        SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                    .lpSecurityDescriptor = nullptr,
                                    .bInheritHandle = TRUE};
        output = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = output;
            startup.hStdError = output;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    PROCESS_INFORMATION info{};
    std::wstring line = command_line(program, {});
    const BOOL created =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr,
                       output != INVALID_HANDLE_VALUE ? TRUE : FALSE, 0, nullptr,
                       folder.wstring().c_str(), &startup, &info);
    if (output != INVALID_HANDLE_VALUE) {
        CloseHandle(output);
    }
    if (created == FALSE) {
        return false;
    }
    CloseHandle(info.hThread);
    if (m_handle != nullptr && m_handle->process != nullptr) {
        CloseHandle(m_handle->process);
    }
    m_handle = std::make_unique<Handle>(Handle{info.hProcess});
    m_exit_code = -1;
    return true;
}

bool Process::running() const {
    if (m_waiting) {
        return true;
    }
    return m_handle != nullptr && m_handle->process != nullptr &&
           WaitForSingleObject(m_handle->process, 0) == WAIT_TIMEOUT;
}

void Process::stop() {
    if (m_handle == nullptr || m_handle->process == nullptr ||
        WaitForSingleObject(m_handle->process, 0) != WAIT_TIMEOUT) {
        return;
    }
    // Asked to close first, as its window's close button would, so it ends as a game should:
    // saving, and writing what it logged. Ended outright only if it doesn't within a few seconds.
    const DWORD id = GetProcessId(m_handle->process);
    EnumWindows(
        [](HWND window, LPARAM target) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == static_cast<DWORD>(target)) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        static_cast<LPARAM>(id));
    if (WaitForSingleObject(m_handle->process, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(m_handle->process, 0);
        WaitForSingleObject(m_handle->process, 5000);
    }
}

#else

struct Process::Handle {
    pid_t process = -1;
};

Process::Process() = default;

namespace {

pid_t spawn(const std::filesystem::path& program, const std::vector<std::string>& arguments,
            const std::filesystem::path& folder, const std::filesystem::path* log) {
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : owned) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        if (chdir(folder.c_str()) != 0) {
            _exit(127);
        }
        if (log != nullptr) {
            const int output = open(log->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (output >= 0) {
                dup2(output, STDOUT_FILENO);
                dup2(output, STDERR_FILENO);
                close(output);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return child;
}

} // namespace

Process::~Process() {
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
}

bool Process::run(const std::filesystem::path& program, const std::vector<std::string>& arguments,
                  const std::filesystem::path& folder, const std::filesystem::path& log) {
    if (running()) {
        return false;
    }
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
    const pid_t child = spawn(program, arguments, folder, &log);
    if (child < 0) {
        return false;
    }
    m_exit_code = -1;
    m_waiting = true;
    m_waiter = std::thread([this, child] {
        int status = 0;
        waitpid(child, &status, 0);
        m_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        m_waiting = false;
    });
    return true;
}

bool Process::start(const std::filesystem::path& program, const std::filesystem::path& folder,
                    const std::filesystem::path& log) {
    if (running()) {
        return false;
    }
    const pid_t child = spawn(program, {}, folder, log.empty() ? nullptr : &log);
    if (child < 0) {
        return false;
    }
    m_handle = std::make_unique<Handle>(Handle{child});
    m_exit_code = -1;
    return true;
}

bool Process::running() const {
    if (m_waiting) {
        return true;
    }
    if (m_handle == nullptr || m_handle->process <= 0) {
        return false;
    }
    int status = 0;
    if (waitpid(m_handle->process, &status, WNOHANG) == 0) {
        return true;
    }
    m_handle->process = -1;
    return false;
}

void Process::stop() {
    if (running() && m_handle != nullptr && m_handle->process > 0) {
        kill(m_handle->process, SIGTERM);
        int status = 0;
        waitpid(m_handle->process, &status, 0);
        m_handle->process = -1;
    }
}

#endif

} // namespace dream::editor
