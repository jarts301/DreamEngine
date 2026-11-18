#pragma once

#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::core {

// A game's assets in one file, as packaging writes them for PC, read without unpacking.
//
// Layout, little-endian: the 8 bytes "DREAMPAK", a version (u32, 1), the file count (u32), and the
// index's offset and size (u64 each). Then each file's bytes, starting at multiples of 16. The
// index lists each file as its offset (u64), size (u64), path length (u16) and path, in UTF-8 with
// forward slashes, sorted by path.

// Reads files from an archive. Move-only; the file stays open while it lives.
class ArchiveReader {
public:
    // NotFound if there is no file at `path`, ParseError if it isn't a valid archive.
    [[nodiscard]] static Result<ArchiveReader> open(std::string_view path);

    ArchiveReader(ArchiveReader&& other) noexcept;
    ArchiveReader& operator=(ArchiveReader&& other) noexcept;
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
    ~ArchiveReader();

    [[nodiscard]] bool contains(std::string_view path) const;

    // NotFound for a path the archive doesn't hold.
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path);

    [[nodiscard]] std::size_t file_count() const noexcept {
        return m_entries.size();
    }

private:
    struct Entry {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    ArchiveReader() = default;

    std::FILE* m_file = nullptr;
    std::map<std::string, Entry, std::less<>> m_entries;
};

// Writes an archive file by file, each written as it is added.
class ArchiveWriter {
public:
    [[nodiscard]] static Result<ArchiveWriter> create(std::string_view path);

    ArchiveWriter(ArchiveWriter&& other) noexcept;
    ArchiveWriter& operator=(ArchiveWriter&&) = delete;
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;
    // An archive not finished is left incomplete, and open refuses it.
    ~ArchiveWriter();

    // `path` is as the game names the file after "assets:/". Each path can be added once.
    [[nodiscard]] Result<void> add(std::string_view path, std::span<const std::byte> data);

    // Writes the index and closes the file.
    [[nodiscard]] Result<void> finish();

private:
    ArchiveWriter() = default;

    std::FILE* m_file = nullptr;
    std::uint64_t m_position = 0;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>> m_entries;
};

} // namespace dream::core
