// Packs a folder of assets into one archive, as a PC game's package carries them. See
// core/include/dream/core/archive.h and docs/adr/0024-texture-cooking.md.
//
// Usage: dream_pack <assets-folder> <archive>

#include "dream/core/archive.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("Usage: dream_pack <assets-folder> <archive>\n", stderr);
        return 2;
    }
    const fs::path folder(argv[1]);

    // Sorted, so the same assets always make the same archive.
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error)) {
            files.push_back(it->path());
        }
    }
    if (error) {
        std::fprintf(stderr, "Could not list '%s': %s\n", argv[1], error.message().c_str());
        return 1;
    }
    std::ranges::sort(files);

    dream::core::Result<dream::core::ArchiveWriter> writer =
        dream::core::ArchiveWriter::create(argv[2]);
    if (!writer.has_value()) {
        std::fprintf(stderr, "%s\n", writer.error().message.c_str());
        return 1;
    }
    std::uintmax_t total = 0;
    for (const fs::path& file : files) {
        const std::u8string relative = file.lexically_relative(folder).generic_u8string();
        const std::string path(relative.begin(), relative.end());
        std::ifstream input(file, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            std::fprintf(stderr, "Could not read '%s'\n", path.c_str());
            return 1;
        }
        if (const auto added = writer.value().add(path, std::as_bytes(std::span(data)));
            !added.has_value()) {
            std::fprintf(stderr, "%s\n", added.error().message.c_str());
            return 1;
        }
        total += data.size();
    }
    if (const auto finished = writer.value().finish(); !finished.has_value()) {
        std::fprintf(stderr, "%s\n", finished.error().message.c_str());
        return 1;
    }
    std::printf("Packed %zu files, %ju bytes, into %s\n", files.size(), total, argv[2]);
    return 0;
}
