#include "dream/core/archive.h"

#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace dream::core {
namespace {

constexpr std::array<char, 8> k_magic{'D', 'R', 'E', 'A', 'M', 'P', 'A', 'K'};
constexpr std::uint32_t k_version = 1;
constexpr std::uint64_t k_header_size = 8 + 4 + 4 + 8 + 8;
constexpr std::uint64_t k_alignment = 16;

Error archive_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

// Paths are UTF-8, which the C library takes everywhere but Windows.
std::FILE* open_file(std::string_view path, const char* mode) {
#if defined(_WIN32)
    const auto convert = [](std::string_view text) {
        const int size =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            size);
        return wide;
    };
    const std::wstring wide_path = convert(path);
    const std::wstring wide_mode = convert(mode);
    std::FILE* file = nullptr;
    return _wfopen_s(&file, wide_path.c_str(), wide_mode.c_str()) == 0 ? file : nullptr;
#else
    return std::fopen(std::string(path).c_str(), mode);
#endif
}

bool seek(std::FILE* file, std::uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

template <typename T> void put(std::vector<std::byte>& out, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (index * 8)) &
                                             0xFF));
    }
}

template <typename T> bool take(std::span<const std::byte>& in, T& value) {
    if (in.size() < sizeof(T)) {
        return false;
    }
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        result |= std::to_integer<std::uint64_t>(in[index]) << (index * 8);
    }
    value = static_cast<T>(result);
    in = in.subspan(sizeof(T));
    return true;
}

} // namespace

Result<ArchiveReader> ArchiveReader::open(std::string_view path) {
    ArchiveReader reader;
    reader.m_file = open_file(path, "rb");
    if (reader.m_file == nullptr) {
        return archive_error(ErrorCode::NotFound, std::format("No archive at '{}'", path));
    }
    const auto invalid = [&] {
        return archive_error(ErrorCode::ParseError,
                             std::format("'{}' is not a valid asset archive", path));
    };

    std::array<std::byte, k_header_size> header{};
    if (std::fread(header.data(), 1, header.size(), reader.m_file) != header.size() ||
        std::memcmp(header.data(), k_magic.data(), k_magic.size()) != 0) {
        return invalid();
    }
    std::span<const std::byte> in = std::span(header).subspan(k_magic.size());
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    take(in, version);
    take(in, count);
    take(in, index_offset);
    take(in, index_size);
    if (version != k_version || index_offset < k_header_size ||
        index_size > std::numeric_limits<std::uint32_t>::max()) {
        return invalid();
    }

    std::vector<std::byte> index(static_cast<std::size_t>(index_size));
    if (!seek(reader.m_file, index_offset) ||
        std::fread(index.data(), 1, index.size(), reader.m_file) != index.size()) {
        return invalid();
    }
    in = index;
    for (std::uint32_t file = 0; file < count; ++file) {
        Entry entry;
        std::uint16_t length = 0;
        if (!take(in, entry.offset) || !take(in, entry.size) || !take(in, length) ||
            in.size() < length || entry.offset < k_header_size ||
            entry.offset + entry.size > index_offset || entry.offset + entry.size < entry.offset) {
            return invalid();
        }
        std::string name(reinterpret_cast<const char*>(in.data()), length);
        in = in.subspan(length);
        if (!reader.m_entries.emplace(std::move(name), entry).second) {
            return invalid();
        }
    }
    if (!in.empty()) {
        return invalid();
    }
    return reader;
}

ArchiveReader::ArchiveReader(ArchiveReader&& other) noexcept
    : m_file(std::exchange(other.m_file, nullptr)), m_entries(std::move(other.m_entries)) {}

ArchiveReader& ArchiveReader::operator=(ArchiveReader&& other) noexcept {
    if (this != &other) {
        if (m_file != nullptr) {
            std::fclose(m_file);
        }
        m_file = std::exchange(other.m_file, nullptr);
        m_entries = std::move(other.m_entries);
    }
    return *this;
}

ArchiveReader::~ArchiveReader() {
    if (m_file != nullptr) {
        std::fclose(m_file);
    }
}

bool ArchiveReader::contains(std::string_view path) const {
    return m_entries.find(path) != m_entries.end();
}

Result<std::vector<std::byte>> ArchiveReader::read(std::string_view path) {
    const auto entry = m_entries.find(path);
    if (entry == m_entries.end()) {
        return archive_error(ErrorCode::NotFound, std::format("No such asset: {}", path));
    }
    if (entry->second.size > std::numeric_limits<std::size_t>::max()) {
        return archive_error(ErrorCode::IoError, std::format("'{}' is too large to read", path));
    }
    std::vector<std::byte> data(static_cast<std::size_t>(entry->second.size));
    if (!seek(m_file, entry->second.offset) ||
        std::fread(data.data(), 1, data.size(), m_file) != data.size()) {
        return archive_error(ErrorCode::IoError, std::format("Could not read '{}' from the asset "
                                                             "archive",
                                                             path));
    }
    return data;
}

Result<ArchiveWriter> ArchiveWriter::create(std::string_view path) {
    ArchiveWriter writer;
    writer.m_file = open_file(path, "wb");
    if (writer.m_file == nullptr) {
        return archive_error(ErrorCode::IoError, std::format("Could not create '{}'", path));
    }
    // The header is written last, once the index's place is known.
    const std::array<std::byte, k_header_size> blank{};
    if (std::fwrite(blank.data(), 1, blank.size(), writer.m_file) != blank.size()) {
        return archive_error(ErrorCode::IoError, std::format("Could not write '{}'", path));
    }
    writer.m_position = k_header_size;
    return writer;
}

ArchiveWriter::ArchiveWriter(ArchiveWriter&& other) noexcept
    : m_file(std::exchange(other.m_file, nullptr)), m_position(other.m_position),
      m_entries(std::move(other.m_entries)) {}

ArchiveWriter::~ArchiveWriter() {
    if (m_file != nullptr) {
        std::fclose(m_file);
    }
}

Result<void> ArchiveWriter::add(std::string_view path, std::span<const std::byte> data) {
    if (m_file == nullptr) {
        return archive_error(ErrorCode::InvalidArgument, "The archive is already finished");
    }
    if (path.empty() || path.size() > std::numeric_limits<std::uint16_t>::max()) {
        return archive_error(ErrorCode::InvalidArgument,
                             std::format("Invalid asset path '{}'", path));
    }
    if (m_entries.contains(path)) {
        return archive_error(ErrorCode::InvalidArgument,
                             std::format("'{}' is in the archive twice", path));
    }
    const std::array<std::byte, k_alignment> padding{};
    const std::uint64_t pad = (k_alignment - m_position % k_alignment) % k_alignment;
    if (std::fwrite(padding.data(), 1, static_cast<std::size_t>(pad), m_file) != pad ||
        std::fwrite(data.data(), 1, data.size(), m_file) != data.size()) {
        return archive_error(ErrorCode::IoError, std::format("Could not write '{}'", path));
    }
    m_position += pad;
    m_entries.emplace(std::string(path), std::pair{m_position, std::uint64_t{data.size()}});
    m_position += data.size();
    return {};
}

Result<void> ArchiveWriter::finish() {
    if (m_file == nullptr) {
        return archive_error(ErrorCode::InvalidArgument, "The archive is already finished");
    }
    std::vector<std::byte> index;
    for (const auto& [path, place] : m_entries) {
        put(index, place.first);
        put(index, place.second);
        put(index, static_cast<std::uint16_t>(path.size()));
        const auto text = std::as_bytes(std::span(path));
        index.insert(index.end(), text.begin(), text.end());
    }
    std::vector<std::byte> header;
    const auto magic = std::as_bytes(std::span(k_magic));
    header.insert(header.end(), magic.begin(), magic.end());
    put(header, k_version);
    put(header, static_cast<std::uint32_t>(m_entries.size()));
    put(header, m_position);
    put(header, std::uint64_t{index.size()});

    const bool written = std::fwrite(index.data(), 1, index.size(), m_file) == index.size() &&
                         seek(m_file, 0) &&
                         std::fwrite(header.data(), 1, header.size(), m_file) == header.size();
    const bool closed = std::fclose(std::exchange(m_file, nullptr)) == 0;
    if (!written || !closed) {
        return archive_error(ErrorCode::IoError, "Could not finish the asset archive");
    }
    return {};
}

} // namespace dream::core
