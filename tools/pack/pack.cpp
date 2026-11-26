// Packs a folder of assets into archives, as a PC game's package carries them. See
// core/include/dream/core/archive.h and docs/adr/0024-texture-cooking.md.
//
// Usage: dream_pack <assets-folder> <archive>
//
// When the folder has the build's asset database and it names bundles (ADR 0047), each bundle's
// files go into <bundle>.pak beside <archive>, and the rest, the base, into <archive>.

#include "dream/core/archive.h"
#include "dream/core/asset_database.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::optional<std::vector<char>> read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return data;
}

// Writes `paths`, under `folder`, into one archive.
bool pack(const fs::path& folder, const std::vector<std::string>& paths, const fs::path& archive) {
    dream::core::Result<dream::core::ArchiveWriter> writer =
        dream::core::ArchiveWriter::create(archive.string());
    if (!writer.has_value()) {
        std::fprintf(stderr, "%s\n", writer.error().message.c_str());
        return false;
    }
    std::uintmax_t total = 0;
    for (const std::string& path : paths) {
        const std::optional<std::vector<char>> data =
            read_file(folder / fs::path(std::u8string(path.begin(), path.end())));
        if (!data.has_value()) {
            std::fprintf(stderr, "Could not read '%s'\n", path.c_str());
            return false;
        }
        if (const auto added = writer.value().add(path, std::as_bytes(std::span(*data)));
            !added.has_value()) {
            std::fprintf(stderr, "%s\n", added.error().message.c_str());
            return false;
        }
        total += data->size();
    }
    if (const auto finished = writer.value().finish(); !finished.has_value()) {
        std::fprintf(stderr, "%s\n", finished.error().message.c_str());
        return false;
    }
    std::printf("Packed %zu files, %ju bytes, into %s\n", paths.size(), total,
                archive.generic_string().c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("Usage: dream_pack <assets-folder> <archive>\n", stderr);
        return 2;
    }
    const fs::path folder(argv[1]);
    const fs::path archive(argv[2]);

    // Sorted, so the same assets always make the same archive.
    std::vector<std::string> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error)) {
            const std::u8string relative = it->path().lexically_relative(folder).generic_u8string();
            files.emplace_back(relative.begin(), relative.end());
        }
    }
    if (error) {
        std::fprintf(stderr, "Could not list '%s': %s\n", argv[1], error.message().c_str());
        return 1;
    }
    std::ranges::sort(files);

    // The bundles each file is shipped in, from the build's database, when it has one.
    std::map<std::string, std::vector<std::string>> bundles;
    std::optional<dream::core::AssetDatabase> database;
    if (const std::optional<std::vector<char>> text = read_file(folder / "asset_database.toml");
        text.has_value() && fs::exists(folder / "asset_database.toml")) {
        dream::core::Result<dream::core::AssetDatabase> parsed = dream::core::AssetDatabase::parse(
            std::string_view(text->data(), text->size()));
        if (!parsed.has_value()) {
            std::fprintf(stderr, "%s\n", parsed.error().message.c_str());
            return 1;
        }
        database = std::move(parsed).value();
    }
    for (const std::string& file : files) {
        const std::string_view bundle =
            database.has_value() ? database->bundle_of(file) : std::string_view("base");
        bundles[std::string(bundle)].push_back(file);
    }
    if (!pack(folder, bundles["base"], archive)) {
        return 1;
    }
    for (const auto& [bundle, paths] : bundles) {
        if (bundle != "base" &&
            !pack(folder, paths, archive.parent_path() / (bundle + ".pak"))) {
            return 1;
        }
    }
    return 0;
}
