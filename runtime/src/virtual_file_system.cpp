#include "dream/runtime/virtual_file_system.h"

#include <format>

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

core::Result<std::vector<std::byte>> VirtualFileSystem::read(std::string_view path) const {
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed.has_value()) {
        return parsed.error();
    }
    return m_platform.read_file(parsed.value().root, parsed.value().relative);
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
