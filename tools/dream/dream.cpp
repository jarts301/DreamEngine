// The engine's command-line tool (ADR 0045). For now it makes new game projects:
//
//     dream new <name> [--path <folder>] [--id <application id>] [--title <title>]
//     dream version
//
// `new` copies the SDK's project template, templates/game, into a folder of the game's own, which
// becomes its repository: it builds with find_package(DreamEngine), from wherever the SDK is.

#include "dream/core/version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
namespace core = dream::core;

namespace {

void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

int fail(std::string_view message) {
    std::fprintf(stderr, "dream: %.*s\n", static_cast<int>(message.size()), message.data());
    return 1;
}

constexpr std::string_view k_usage =
    "Usage:\n"
    "  dream new <name> [--path <folder>] [--id <application id>] [--title <title>]\n"
    "      Makes a game project in <folder>, ./<name> by default. <name> is lowercase letters,\n"
    "      digits and underscores, starting with a letter. The application ID, which Android\n"
    "      and the stores use, is com.example.<name> unless given.\n"
    "  dream version\n"
    "      Prints the engine's version.\n";

std::optional<fs::path> executable_path() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    return fs::path(buffer.c_str());
#else
    std::error_code error;
    fs::path path = fs::read_symlink("/proc/self/exe", error);
    if (error) {
        return std::nullopt;
    }
    return path;
#endif
}

// An environment variable holding a path; Windows' is read as UTF-16, for any folder name.
std::optional<fs::path> environment_path([[maybe_unused]] const wchar_t* wide_name,
                                         [[maybe_unused]] const char* name) {
#ifdef _WIN32
    const DWORD size = GetEnvironmentVariableW(wide_name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide_name, value.data(), size);
    if (length == 0 || length >= size) {
        return std::nullopt;
    }
    value.resize(length);
    return fs::path(value);
#else
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return fs::path(value);
#endif
}

bool is_sdk(const fs::path& folder) {
    std::error_code error;
    return fs::exists(folder / "templates" / "game" / "CMakeLists.txt", error) &&
           fs::exists(folder / "cmake" / "DreamEngineConfig.cmake", error);
}

// The SDK this tool belongs to: the folder above its bin folder when it is installed, the engine's
// checkout when it was built there, or else DREAM_ENGINE_SDK.
std::optional<fs::path> find_sdk() {
    if (const std::optional<fs::path> self = executable_path()) {
        const fs::path above = self->parent_path().parent_path();
        if (is_sdk(above)) {
            return above;
        }
    }
#ifdef DREAM_ENGINE_SOURCE_DIR
    if (is_sdk(DREAM_ENGINE_SOURCE_DIR)) {
        return fs::path(DREAM_ENGINE_SOURCE_DIR);
    }
#endif
    if (const std::optional<fs::path> variable =
            environment_path(L"DREAM_ENGINE_SDK", "DREAM_ENGINE_SDK");
        variable.has_value() && is_sdk(*variable)) {
        return variable;
    }
    return std::nullopt;
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 40 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Dot-separated parts, each starting with a letter, of letters, digits and underscores, as
// Android requires; at least two parts.
bool valid_application_id(std::string_view id) {
    std::size_t parts = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t dot = std::min(id.find('.', start), id.size());
        const std::string_view part = id.substr(start, dot - start);
        if (part.empty() || std::isalpha(static_cast<unsigned char>(part.front())) == 0 ||
            !std::ranges::all_of(part, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
            })) {
            return false;
        }
        ++parts;
        start = dot + 1;
    }
    return parts >= 2;
}

// "my_game" becomes "My Game".
std::string title_from(std::string_view name) {
    std::string title;
    bool word_start = true;
    for (const char c : name) {
        if (c == '_') {
            title += ' ';
            word_start = true;
        } else {
            title += word_start
                         ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                         : c;
            word_start = false;
        }
    }
    return title;
}

// Files whose text carries the template's placeholders. Others, such as models and images, are
// copied byte for byte.
bool is_text(const fs::path& path) {
    const std::string extension = path.extension().string();
    const std::string name = path.filename().string();
    constexpr std::string_view k_text[] = {".txt", ".cmake", ".json", ".toml", ".cpp", ".h",
                                           ".md",  ".rml",   ".rcss", ".slang"};
    return name == ".gitignore" || name == ".gitattributes" ||
           std::ranges::find(k_text, extension) != std::end(k_text);
}

std::string replace_all(std::string text,
                        const std::vector<std::pair<std::string, std::string>>& replacements) {
    for (const auto& [from, to] : replacements) {
        for (std::size_t at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    return text;
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

// The game's vcpkg.json: the SDK's own, which lists the engine's libraries at the versions the
// engine is tested with, named for the game, without the engine's test framework.
std::optional<std::string> game_manifest(const fs::path& sdk, std::string_view package) {
    std::optional<std::string> text = read_text(sdk / "vcpkg.json");
    if (!text.has_value()) {
        return std::nullopt;
    }
    const std::vector<std::pair<std::string_view, std::string>> edits{
        {"\"name\": \"dream-engine\"", std::format("\"name\": \"{}\"", package)},
        {"    \"doctest\",\n", ""},
    };
    std::string& manifest = *text;
    std::erase(manifest, '\r');
    for (const auto& [from, to] : edits) {
        const std::size_t at = manifest.find(from);
        if (at == std::string::npos) {
            return std::nullopt;
        }
        manifest.replace(at, from.size(), to);
    }
    const std::size_t version = manifest.find("\"version-semver\": \"");
    if (version == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t start = version + std::string_view("\"version-semver\": \"").size();
    manifest.replace(start, manifest.find('"', start) - start, "0.1.0");
    return manifest;
}

int make_project(const std::vector<std::string_view>& arguments) {
    if (arguments.empty()) {
        return fail("`new` needs the game's name. Run `dream help`.");
    }
    const std::string name(arguments[0]);
    if (!valid_name(name)) {
        return fail(std::format("'{}' can't be a game's name: use lowercase letters, digits and "
                                "underscores, starting with a letter, at most 40 in all",
                                name));
    }
    fs::path folder = name;
    std::string application_id = "com.example." + name;
    std::string title = title_from(name);
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view option = arguments[index];
        if (index + 1 >= arguments.size()) {
            return fail(std::format("'{}' needs a value", option));
        }
        const std::string_view value = arguments[++index];
        if (option == "--path") {
            folder = fs::path(std::string(value));
        } else if (option == "--id") {
            application_id = value;
        } else if (option == "--title") {
            title = value;
        } else {
            return fail(std::format("Unknown option '{}'. Run `dream help`.", option));
        }
    }
    if (!valid_application_id(application_id)) {
        return fail(std::format("'{}' isn't an application ID: it needs two or more parts "
                                "separated by dots, such as com.studio.game, each starting with a "
                                "letter",
                                application_id));
    }
    if (title.empty() || title.find_first_of("\"\\\n\r") != std::string::npos) {
        return fail("The title can't be empty, or hold quotes, backslashes or line breaks");
    }

    const std::optional<fs::path> sdk = find_sdk();
    if (!sdk.has_value()) {
        return fail("The SDK's project template can't be found. Run the `dream` in an SDK's bin "
                    "folder, or set DREAM_ENGINE_SDK to the SDK's folder.");
    }
    const fs::path source = *sdk / "templates" / "game";

    std::error_code error;
    if (fs::exists(folder, error) && !fs::is_empty(folder, error)) {
        return fail(std::format("'{}' already exists and isn't empty", folder.string()));
    }

    const core::Version version = core::engine_version();
    const std::vector<std::pair<std::string, std::string>> replacements{
        {"__GAME_NAME__", name},
        {"__GAME_TITLE__", title},
        {"__APPLICATION_ID__", application_id},
        {"__ENGINE_VERSION__", std::format("{}.{}", version.major_number, version.minor_number)},
    };

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(source, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error)) {
            files.push_back(it->path());
        }
    }
    if (error) {
        return fail(std::format("Could not read the template in '{}': {}", source.string(),
                                error.message()));
    }
    std::ranges::sort(files);
    for (const fs::path& file : files) {
        const fs::path relative = fs::relative(file, source, error);
        const fs::path target = folder / replace_all(relative.generic_string(), replacements);
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return fail(std::format("Could not make '{}': {}", target.parent_path().string(),
                                    error.message()));
        }
        if (is_text(file)) {
            const std::optional<std::string> text = read_text(file);
            if (!text.has_value() || !write_text(target, replace_all(*text, replacements))) {
                return fail(std::format("Could not write '{}'", target.string()));
            }
        } else if (!fs::copy_file(file, target, fs::copy_options::overwrite_existing, error)) {
            return fail(std::format("Could not write '{}': {}", target.string(), error.message()));
        }
    }

    // vcpkg names are lowercase letters, digits and dashes.
    std::string package = name;
    std::ranges::replace(package, '_', '-');
    const std::optional<std::string> manifest = game_manifest(*sdk, package);
    if (!manifest.has_value() || !write_text(folder / "vcpkg.json", *manifest)) {
        return fail(std::format("Could not make vcpkg.json from '{}'",
                                (*sdk / "vcpkg.json").string()));
    }

    const fs::path sdk_path = fs::absolute(*sdk, error).lexically_normal();
    print(std::format(
        "Made '{}' in {}, from the SDK in {}.\n"
        "\n"
        "Its presets find the SDK and vcpkg through two environment variables:\n"
        "  DREAM_ENGINE_SDK = {}\n"
        "  VCPKG_ROOT       = your vcpkg folder\n"
        "\n"
        "Then, in that folder:\n"
        "  cmake --preset windows-debug\n"
        "  cmake --build --preset windows-debug\n"
        "\n"
        "It is ready to be a repository of its own: `git init` there. See its README.md.\n",
        title, folder.string(), sdk_path.string(), sdk_path.string()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    if (arguments.empty() || arguments[0] == "help" || arguments[0] == "--help" ||
        arguments[0] == "-h") {
        print(k_usage);
        return arguments.empty() ? 2 : 0;
    }
    if (arguments[0] == "version" || arguments[0] == "--version") {
        const core::Version version = core::engine_version();
        print(std::format("DreamEngine {}.{}.{}\n", version.major_number, version.minor_number,
                          version.patch_number));
        return 0;
    }
    if (arguments[0] == "new") {
        return make_project({arguments.begin() + 1, arguments.end()});
    }
    return fail(std::format("Unknown command '{}'. Run `dream help`.", arguments[0]));
}
