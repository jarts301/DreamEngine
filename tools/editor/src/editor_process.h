#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dream::editor {

// A program the editor runs: a build, which it waits for on a thread of its own, or the game,
// which it starts and can stop. Windows and Linux.
class Process {
public:
    Process();
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Runs `program` with `arguments` in `folder` and waits for it on a thread, writing what it
    // prints to `log`. Fails if one is already running.
    bool run(const std::filesystem::path& program, const std::vector<std::string>& arguments,
             const std::filesystem::path& folder, const std::filesystem::path& log);

    // Starts `program` in `folder`, its own folder, without waiting, writing what it prints to
    // `log` when one is given. Fails if one is running.
    bool start(const std::filesystem::path& program, const std::filesystem::path& folder,
               const std::filesystem::path& log = {});

    // Whether it is still running.
    [[nodiscard]] bool running() const;
    // Its exit code once it ended, or -1.
    [[nodiscard]] int exit_code() const noexcept {
        return m_exit_code.load();
    }
    // Asks it to close, and ends it if it doesn't within a few seconds, when it was started rather
    // than run.
    void stop();

private:
    struct Handle;
    std::unique_ptr<Handle> m_handle;
    std::thread m_waiter;
    std::atomic<bool> m_waiting{false};
    std::atomic<int> m_exit_code{-1};
};

} // namespace dream::editor
