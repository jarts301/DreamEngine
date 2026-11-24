#include "dream/runtime/virtual_file_system.h"

#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace dream::runtime {
namespace {

using platform::FileRoot;

struct ParsedPath {
    FileRoot root;
    std::string_view relative;
};

core::Error invalid_path(std::string_view path, std::string_view reason) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Invalid path '{}': {}", path, reason)};
}

bool is_safe_relative_path(std::string_view path) {
    if (path.empty() || path.find_first_of("\\:") != std::string_view::npos) {
        return false;
    }

    while (true) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

core::Result<ParsedPath> parse_path(std::string_view path) {
    constexpr std::string_view k_assets = "assets:/";
    constexpr std::string_view k_user = "user:/";

    ParsedPath parsed{};
    if (path.starts_with(k_assets)) {
        parsed = ParsedPath{FileRoot::Assets, path.substr(k_assets.size())};
    } else if (path.starts_with(k_user)) {
        parsed = ParsedPath{FileRoot::User, path.substr(k_user.size())};
    } else {
        return invalid_path(path, "it must start with assets:/ or user:/");
    }

    if (!is_safe_relative_path(parsed.relative)) {
        return invalid_path(path, "it contains an empty or unsafe part");
    }
    return parsed;
}

} // namespace

void VirtualFileSystem::add_asset_folder(std::filesystem::path folder) {
    m_asset_folders.push_back(std::move(folder));
}

std::optional<std::filesystem::path>
VirtualFileSystem::in_asset_folders(std::string_view relative) const {
    for (auto folder = m_asset_folders.rbegin(); folder != m_asset_folders.rend(); ++folder) {
        std::filesystem::path file = *folder / std::filesystem::path(std::u8string(
                                                   relative.begin(), relative.end()));
        std::error_code error;
        if (std::filesystem::is_regular_file(file, error)) {
            return file;
        }
    }
    return std::nullopt;
}

core::Result<std::vector<std::byte>> VirtualFileSystem::read(std::string_view path) const {
    if (m_read_observer) {
        m_read_observer(path);
    }
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed.has_value()) {
        return parsed.error();
    }
    if (parsed.value().root == FileRoot::Assets && !m_asset_folders.empty()) {
        if (const std::optional<std::filesystem::path> file =
                in_asset_folders(parsed.value().relative)) {
            std::ifstream in(*file, std::ios::binary);
            std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            if (!in.bad()) {
                std::vector<std::byte> bytes(chars.size());
                for (std::size_t index = 0; index < chars.size(); ++index) {
                    bytes[index] = static_cast<std::byte>(chars[index]);
                }
                return bytes;
            }
            return core::Error{core::ErrorCode::IoError, std::format("Could not read '{}'", path)};
        }
    }
    return m_platform.read_file(parsed.value().root, parsed.value().relative);
}

std::optional<std::int64_t> VirtualFileSystem::version(std::string_view path) const {
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (parsed.value().root == FileRoot::Assets && !m_asset_folders.empty()) {
        if (const std::optional<std::filesystem::path> file =
                in_asset_folders(parsed.value().relative)) {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(*file, error);
            if (!error) {
                return static_cast<std::int64_t>(time.time_since_epoch().count());
            }
        }
    }
    return m_platform.file_version(parsed.value().root, parsed.value().relative);
}

core::Result<void> VirtualFileSystem::write(std::string_view path,
                                            std::span<const std::byte> data) const {
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed.has_value()) {
        return parsed.error();
    }
    if (parsed.value().root != FileRoot::User) {
        return invalid_path(path, "assets are read-only");
    }
    return m_platform.write_user_file(parsed.value().relative, data);
}

} // namespace dream::runtime
