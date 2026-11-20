// The hub: one web page from which the engine is built, its games started and played, and its
// tests run and read, on a machine with no display (ADR 0039).
//
//   dream_hub [--host 127.0.0.1] [--port 8000] [--build <build folder>]
//
// Then open http://127.0.0.1:8000/ in a web browser. Each game it starts serves its own page (the
// web platform), on a port the hub picks, and the hub links to it. Use --host 0.0.0.0 to reach the
// hub from other machines on a trusted network: anyone who can open it can start the builds, games
// and tests listed here, and nothing else.

#include "hub_page.h"

#include "webserve/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// The most output kept for one process: its end, which is what matters.
constexpr std::size_t k_max_log_bytes = 512 * 1024;
constexpr std::uint16_t k_first_game_port = 8100;

std::atomic<bool> g_stop{false};

void on_signal(int /*signal*/) {
    g_stop = true;
}

// A program the hub started, and what it printed.
class Process {
public:
    Process(std::string label, std::vector<std::string> arguments, std::string directory,
            std::vector<std::pair<std::string, std::string>> environment)
        : m_label(std::move(label)) {
        int pipe_ends[2];
        if (::pipe2(pipe_ends, O_CLOEXEC) != 0) {
            append(std::format("Could not start: {}\n", std::strerror(errno)));
            m_finished = true;
            return;
        }
        const pid_t child = ::fork();
        if (child == 0) {
            ::dup2(pipe_ends[1], STDOUT_FILENO);
            ::dup2(pipe_ends[1], STDERR_FILENO);
            ::setpgid(0, 0);
            if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
                std::fprintf(stderr, "Could not enter %s\n", directory.c_str());
                ::_exit(127);
            }
            for (const auto& [name, value] : environment) {
                ::setenv(name.c_str(), value.c_str(), 1);
            }
            std::vector<char*> argv;
            for (std::string& argument : arguments) {
                argv.push_back(argument.data());
            }
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            std::fprintf(stderr, "Could not run %s: %s\n", argv[0], std::strerror(errno));
            ::_exit(127);
        }
        ::close(pipe_ends[1]);
        if (child < 0) {
            ::close(pipe_ends[0]);
            append(std::format("Could not start: {}\n", std::strerror(errno)));
            m_finished = true;
            return;
        }
        m_pid = child;
        m_started = std::chrono::steady_clock::now();
        m_reader = std::thread([this, fd = pipe_ends[0]] { read_output(fd); });
    }

    ~Process() {
        stop();
        // A few seconds to stop cleanly, then it is killed.
        for (int wait = 0; wait < 30 && !finished(); ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            const std::lock_guard lock(m_mutex);
            if (!m_finished && m_pid > 0) {
                ::kill(-m_pid, SIGKILL);
            }
        }
        if (m_reader.joinable()) {
            m_reader.join();
        }
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    void stop() {
        const std::lock_guard lock(m_mutex);
        if (!m_finished && m_pid > 0) {
            ::kill(-m_pid, SIGTERM);
            m_stop_asked = std::chrono::steady_clock::now();
        }
    }

    // Kills a process that ignored stop for a few seconds.
    void enforce_stop() {
        const std::lock_guard lock(m_mutex);
        if (!m_finished && m_pid > 0 && m_stop_asked.has_value() &&
            std::chrono::steady_clock::now() - *m_stop_asked > std::chrono::seconds(3)) {
            ::kill(-m_pid, SIGKILL);
        }
    }

    [[nodiscard]] bool finished() const {
        const std::lock_guard lock(m_mutex);
        return m_finished;
    }

    // "running", or how it ended.
    [[nodiscard]] std::string state() const {
        const std::lock_guard lock(m_mutex);
        if (!m_finished) {
            return "running";
        }
        if (m_signal != 0) {
            return std::format("stopped by signal {}", m_signal);
        }
        return std::format("exited with code {}", m_exit_code);
    }

    [[nodiscard]] int exit_code() const {
        const std::lock_guard lock(m_mutex);
        return m_finished && m_signal == 0 ? m_exit_code : -1;
    }

    [[nodiscard]] double seconds() const {
        const std::lock_guard lock(m_mutex);
        const auto end = m_finished ? m_ended : std::chrono::steady_clock::now();
        return std::chrono::duration<double>(end - m_started).count();
    }

    [[nodiscard]] std::string log() const {
        const std::lock_guard lock(m_mutex);
        return m_log;
    }

    [[nodiscard]] const std::string& label() const {
        return m_label;
    }

private:
    void append(std::string_view text) {
        m_log.append(text);
        if (m_log.size() > k_max_log_bytes) {
            m_log.erase(0, m_log.size() - k_max_log_bytes);
        }
    }

    void read_output(int fd) {
        std::array<char, 8192> buffer{};
        while (true) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            const std::lock_guard lock(m_mutex);
            append(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
        ::close(fd);
        int status = 0;
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
        }
        const std::lock_guard lock(m_mutex);
        m_finished = true;
        m_ended = std::chrono::steady_clock::now();
        if (WIFEXITED(status)) {
            m_exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m_signal = WTERMSIG(status);
        }
    }

    std::string m_label;
    mutable std::mutex m_mutex;
    pid_t m_pid = -1;
    std::string m_log;
    bool m_finished = false;
    int m_exit_code = 0;
    int m_signal = 0;
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_ended;
    std::optional<std::chrono::steady_clock::time_point> m_stop_asked;
    std::thread m_reader;
};

struct Game {
    std::string name;
    std::string program;
    std::string title;
    std::uint16_t port = 0;
    std::unique_ptr<Process> process;
};

struct TestResult {
    std::string name;
    std::string status;
    double seconds = 0.0;
    std::string output;
};

std::string xml_unescape(std::string_view text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') {
            result.push_back(text[index]);
            continue;
        }
        const std::size_t end = text.find(';', index);
        if (end == std::string_view::npos) {
            result.push_back('&');
            continue;
        }
        const std::string_view entity = text.substr(index + 1, end - index - 1);
        if (entity == "lt") {
            result.push_back('<');
        } else if (entity == "gt") {
            result.push_back('>');
        } else if (entity == "amp") {
            result.push_back('&');
        } else if (entity == "quot") {
            result.push_back('"');
        } else if (entity == "apos") {
            result.push_back('\'');
        } else if (entity.starts_with('#')) {
            const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            const unsigned long code = std::strtoul(std::string(entity.substr(hex ? 2 : 1)).c_str(),
                                                    nullptr, hex ? 16 : 10);
            result.push_back(code < 128 ? static_cast<char>(code) : '?');
        } else {
            result.append(text.substr(index, end - index + 1));
        }
        index = end;
    }
    return result;
}

std::string attribute(std::string_view tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const std::size_t start = tag.find(key);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t value_start = start + key.size();
    const std::size_t end = tag.find('"', value_start);
    return xml_unescape(tag.substr(value_start, end - value_start));
}

// Reads the JUnit XML that ctest --output-junit writes.
std::vector<TestResult> read_junit(const fs::path& file) {
    std::ifstream in(file);
    std::stringstream contents;
    contents << in.rdbuf();
    const std::string xml = contents.str();
    std::vector<TestResult> results;
    std::size_t position = 0;
    while ((position = xml.find("<testcase", position)) != std::string::npos) {
        const std::size_t tag_end = xml.find('>', position);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string_view tag(xml.data() + position, tag_end - position);
        std::size_t case_end = tag_end + 1;
        std::string_view body;
        if (xml[tag_end - 1] != '/') {
            const std::size_t close = xml.find("</testcase>", tag_end);
            body = std::string_view(xml).substr(tag_end + 1, close - tag_end - 1);
            case_end = close == std::string::npos ? xml.size() : close + 11;
        }
        TestResult result{.name = attribute(tag, "name"),
                          .seconds = std::strtod(attribute(tag, "time").c_str(), nullptr)};
        const std::string status = attribute(tag, "status");
        if (body.find("<failure") != std::string_view::npos ||
            body.find("<error") != std::string_view::npos || status == "fail") {
            result.status = "failed";
        } else if (body.find("<skipped") != std::string_view::npos || status == "notrun" ||
                   status == "disabled") {
            result.status = "skipped";
        } else {
            result.status = "passed";
        }
        const std::size_t out_start = body.find("<system-out>");
        if (out_start != std::string_view::npos) {
            const std::size_t out_end = body.find("</system-out>", out_start);
            std::string_view output = body.substr(out_start + 12, out_end - out_start - 12);
            if (output.starts_with("<![CDATA[")) {
                output.remove_prefix(9);
                const std::size_t cdata_end = output.rfind("]]>");
                result.output = std::string(output.substr(0, cdata_end));
            } else {
                result.output = xml_unescape(output);
            }
        }
        results.push_back(std::move(result));
        position = case_end;
    }
    return results;
}

bool port_free(const std::string& host, std::uint16_t port) {
    const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) {
        return false;
    }
    const int yes = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &address.sin_addr);
    const bool free = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(socket);
    return free;
}

class Hub {
public:
    Hub(std::string host, fs::path build, fs::path source)
        : m_host(std::move(host)), m_build(std::move(build)), m_source(std::move(source)) {
        load_games();
    }

    webserve::Response handle(const webserve::Request& request) {
        const std::lock_guard lock(m_mutex);
        if (request.path == "/" || request.path == "/index.html") {
            return webserve::Response{.content_type = "text/html; charset=utf-8",
                                      .body = std::string(hub::k_page)};
        }
        if (request.path == "/api/state") {
            return json(state());
        }
        if (request.method == "POST" && request.path == "/api/game/start") {
            return start_game(webserve::query_value(request.query, "name").value_or(""));
        }
        if (request.method == "POST" && request.path == "/api/game/stop") {
            for (Game& game : m_games) {
                if (game.name == webserve::query_value(request.query, "name") && game.process) {
                    game.process->stop();
                }
            }
            return json(state());
        }
        if (request.path == "/api/log") {
            return log(webserve::query_value(request.query, "of").value_or(""));
        }
        if (request.method == "POST" && request.path == "/api/build") {
            return run_task("build", {m_cmake, "--build", m_build.string(), "--parallel",
                                      std::to_string(jobs())});
        }
        if (request.method == "POST" && request.path == "/api/tests/run") {
            std::vector<std::string> arguments{m_ctest, "--test-dir", m_build.string(),
                                               "--output-on-failure", "--output-junit",
                                               results_file().string(), "--parallel",
                                               std::to_string(jobs())};
            const std::string filter = webserve::query_value(request.query, "filter").value_or("");
            if (!filter.empty()) {
                arguments.push_back("--tests-regex");
                arguments.push_back(filter);
            }
            std::error_code ignored;
            fs::remove(results_file(), ignored);
            return run_task("tests", std::move(arguments));
        }
        if (request.path == "/api/tests/results") {
            return test_results();
        }
        if (request.path == "/api/screenshots") {
            return screenshots();
        }
        if (request.path == "/file") {
            return file(webserve::query_value(request.query, "path").value_or(""));
        }
        return webserve::Response{.status = 404, .body = "Not found"};
    }

    // Called every second or so.
    void tick() {
        const std::lock_guard lock(m_mutex);
        for (Game& game : m_games) {
            if (game.process) {
                game.process->enforce_stop();
            }
        }
    }

    void stop_all() {
        const std::lock_guard lock(m_mutex);
        for (Game& game : m_games) {
            game.process.reset();
        }
        m_task.reset();
    }

private:
    static webserve::Response json(std::string body) {
        return webserve::Response{.content_type = "application/json", .body = std::move(body)};
    }

    static unsigned jobs() {
        return std::max(1U, std::thread::hardware_concurrency());
    }

    [[nodiscard]] fs::path results_file() const {
        return m_build / "hub_test_results.xml";
    }

    void load_games() {
        std::ifstream list(m_build / "hub_games.txt");
        std::string line;
        while (std::getline(list, line)) {
            const std::size_t first = line.find('|');
            const std::size_t second = line.find('|', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                continue;
            }
            m_games.push_back(Game{.name = line.substr(0, first),
                                   .program = line.substr(first + 1, second - first - 1),
                                   .title = line.substr(second + 1)});
        }
    }

    std::string state() const {
        std::string games;
        for (const Game& game : m_games) {
            if (!games.empty()) {
                games += ",";
            }
            std::error_code error;
            const bool built = fs::exists(game.program, error);
            games += std::format(
                R"({{"name":"{}","title":"{}","built":{},"state":"{}","port":{},"running":{}}})",
                webserve::json_escape(game.name), webserve::json_escape(game.title),
                built ? "true" : "false",
                webserve::json_escape(game.process ? game.process->state() : "not started"),
                game.port, game.process && !game.process->finished() ? "true" : "false");
        }
        std::string task = "null";
        if (m_task) {
            task = std::format(R"({{"kind":"{}","state":"{}","seconds":{:.1f},"code":{}}})",
                               m_task->label(), webserve::json_escape(m_task->state()),
                               m_task->seconds(), m_task->exit_code());
        }
        return std::format(R"({{"games":[{}],"task":{},"build":"{}","source":"{}"}})", games, task,
                           webserve::json_escape(m_build.string()),
                           webserve::json_escape(m_source.string()));
    }

    webserve::Response start_game(const std::string& name) {
        for (Game& game : m_games) {
            if (game.name != name) {
                continue;
            }
            if (game.process && !game.process->finished()) {
                return json(state());
            }
            if (!fs::exists(game.program)) {
                return webserve::Response{.status = 409, .body = "Build the game first"};
            }
            // Each game keeps its port, so an open tab finds it again after a restart.
            if (game.port == 0 || !port_free(m_host, game.port)) {
                std::uint16_t port = k_first_game_port;
                while (port < k_first_game_port + 500 &&
                       (!port_free(m_host, port) ||
                        std::ranges::any_of(m_games, [&](const Game& other) {
                            return &other != &game && other.port == port;
                        }))) {
                    ++port;
                }
                game.port = port;
            }
            game.process = std::make_unique<Process>(
                game.name, std::vector<std::string>{game.program},
                fs::path(game.program).parent_path().string(),
                std::vector<std::pair<std::string, std::string>>{
                    {"DREAM_WEB_PORT", std::to_string(game.port)}, {"DREAM_WEB_HOST", m_host}});
            return json(state());
        }
        return webserve::Response{.status = 404, .body = "No such game"};
    }

    webserve::Response run_task(std::string kind, std::vector<std::string> arguments) {
        if (m_task && !m_task->finished()) {
            return webserve::Response{.status = 409,
                                      .body = "Wait for the " + m_task->label() + " to finish"};
        }
        m_task = std::make_unique<Process>(std::move(kind), std::move(arguments), m_build.string(),
                                           std::vector<std::pair<std::string, std::string>>{});
        return json(state());
    }

    webserve::Response log(const std::string& of) const {
        if (of == "task") {
            return webserve::Response{.body = m_task ? m_task->log() : std::string()};
        }
        for (const Game& game : m_games) {
            if (game.name == of) {
                return webserve::Response{.body = game.process ? game.process->log() : std::string()};
            }
        }
        return webserve::Response{.status = 404, .body = "No such log"};
    }

    webserve::Response test_results() const {
        std::error_code error;
        if (!fs::exists(results_file(), error)) {
            return json(R"({"available":false,"tests":[]})");
        }
        std::string tests;
        for (const TestResult& result : read_junit(results_file())) {
            if (!tests.empty()) {
                tests += ",";
            }
            tests += std::format(R"({{"name":"{}","status":"{}","seconds":{:.2f},"output":"{}"}})",
                                 webserve::json_escape(result.name), result.status, result.seconds,
                                 webserve::json_escape(result.output));
        }
        return json(std::format(R"({{"available":true,"tests":[{}]}})", tests));
    }

    // Screenshots that screenshot tests saved in the build folder, beside the references they were
    // compared with.
    webserve::Response screenshots() const {
        std::string items;
        std::error_code error;
        for (auto entry = fs::recursive_directory_iterator(
                 m_build, fs::directory_options::skip_permission_denied, error);
             !error && entry != fs::recursive_directory_iterator(); entry.increment(error)) {
            const fs::path& path = entry->path();
            const std::string name = path.filename().string();
            if (!entry->is_regular_file() || path.extension() != ".png" ||
                name.find("screenshot") == std::string::npos ||
                path.string().find("/assets/") != std::string::npos) {
                continue;
            }
            std::string reference;
            // dream_<game>_..._screenshot.png is compared with the game's reference image, which
            // the test's configuration names; the hub shows each reference of that sample.
            const fs::path relative = fs::relative(path, m_build);
            std::string sample;
            if (relative.begin() != relative.end() && relative.begin()->string() == "samples") {
                auto part = relative.begin();
                ++part;
                if (part != relative.end()) {
                    sample = part->string();
                }
            }
            if (!sample.empty()) {
                const fs::path folder = m_source / "samples" / sample / "tests/reference/screenshots";
                for (auto ref = fs::directory_iterator(folder, error);
                     !error && ref != fs::directory_iterator(); ref.increment(error)) {
                    if (ref->path().extension() == ".png") {
                        reference = fs::relative(ref->path(), m_source).string();
                        break;
                    }
                }
                error.clear();
            }
            if (!items.empty()) {
                items += ",";
            }
            items += std::format(R"({{"capture":"build/{}","reference":"{}","sample":"{}"}})",
                                 webserve::json_escape(relative.string()),
                                 reference.empty() ? std::string()
                                                   : "source/" + webserve::json_escape(reference),
                                 webserve::json_escape(sample));
        }
        return json("[" + items + "]");
    }

    // A PNG from the build or source folder, never from anywhere else.
    webserve::Response file(const std::string& path) const {
        fs::path root;
        std::string relative;
        if (path.starts_with("build/")) {
            root = m_build;
            relative = path.substr(6);
        } else if (path.starts_with("source/")) {
            root = m_source;
            relative = path.substr(7);
        } else {
            return webserve::Response{.status = 404, .body = "Not found"};
        }
        std::error_code error;
        const fs::path base = fs::weakly_canonical(root, error);
        const fs::path full = fs::weakly_canonical(root / relative, error);
        const std::string base_text = base.string() + "/";
        if (error || !full.string().starts_with(base_text) || full.extension() != ".png") {
            return webserve::Response{.status = 404, .body = "Not found"};
        }
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            return webserve::Response{.status = 404, .body = "Not found"};
        }
        std::stringstream contents;
        contents << in.rdbuf();
        return webserve::Response{.content_type = "image/png", .body = contents.str()};
    }

    std::mutex m_mutex;
    std::string m_host;
    fs::path m_build;
    fs::path m_source;
    std::string m_cmake = DREAM_HUB_CMAKE;
    std::string m_ctest = DREAM_HUB_CTEST;
    std::vector<Game> m_games;
    std::unique_ptr<Process> m_task;
};

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8000;
    fs::path build = DREAM_HUB_BUILD_DIR;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--host" && has_value) {
            host = argv[++index];
        } else if (argument == "--port" && has_value) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++index]));
        } else if (argument == "--build" && has_value) {
            build = argv[++index];
        } else {
            std::cout << "Usage: dream_hub [--host 127.0.0.1] [--port 8000] [--build <folder>]\n";
            return argument == "--help" || argument == "-h" ? 0 : 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    Hub hub(host, fs::absolute(build), fs::path(DREAM_HUB_SOURCE_DIR));
    webserve::Server server;
    if (const std::optional<std::string> error =
            server.start(host, port, [&hub](const webserve::Request& request) {
                return hub.handle(request);
            })) {
        std::cerr << "dream_hub: " << *error << "\n";
        return 1;
    }
    std::cout << "DreamEngine hub: open http://" << (host == "0.0.0.0" ? "<this machine>" : host)
              << ":" << server.port() << "/ in a web browser. Ctrl+C stops it and its games."
              << std::endl;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        hub.tick();
    }
    std::cout << "Stopping the hub and the games it started" << std::endl;
    server.stop();
    hub.stop_all();
    return 0;
}
