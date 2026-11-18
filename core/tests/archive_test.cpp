#include "dream/core/archive.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <system_error>
#include <vector>

using namespace dream::core;
namespace fs = std::filesystem;

namespace {

// A file in the system's temporary folder, removed when done.
class TemporaryFile {
public:
    explicit TemporaryFile(const char* name) {
        std::error_code error;
        m_path = fs::temp_directory_path(error) / name;
        fs::remove(m_path, error);
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    ~TemporaryFile() {
        std::error_code error;
        fs::remove(m_path, error);
    }

    [[nodiscard]] std::string path() const {
        return m_path.generic_string();
    }

private:
    fs::path m_path;
};

std::vector<std::byte> bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

void write_raw(const std::string& path, const std::vector<std::byte>& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

std::vector<std::byte> read_raw(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    const std::vector<char> data((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
    const auto view = std::as_bytes(std::span(data));
    return {view.begin(), view.end()};
}

} // namespace

TEST_CASE("An archive gives back the files written to it") {
    const TemporaryFile file("dream_archive_test.pak");
    {
        Result<ArchiveWriter> writer = ArchiveWriter::create(file.path());
        REQUIRE(writer.has_value());
        REQUIRE(writer.value().add("models/ball.glb", bytes("a ball")).has_value());
        REQUIRE(writer.value().add("shaders/forward.vert.spv", bytes("spirv")).has_value());
        REQUIRE(writer.value().add("empty.txt", {}).has_value());
        CHECK_FALSE(writer.value().add("models/ball.glb", bytes("again")).has_value());
        CHECK_FALSE(writer.value().add("", bytes("nameless")).has_value());
        REQUIRE(writer.value().finish().has_value());
        CHECK_FALSE(writer.value().finish().has_value());
    }

    Result<ArchiveReader> reader = ArchiveReader::open(file.path());
    REQUIRE_MESSAGE(reader.has_value(), reader.error().message);
    CHECK(reader.value().file_count() == 3);
    CHECK(reader.value().contains("models/ball.glb"));
    CHECK_FALSE(reader.value().contains("models"));
    CHECK(reader.value().read("shaders/forward.vert.spv").value() == bytes("spirv"));
    CHECK(reader.value().read("models/ball.glb").value() == bytes("a ball"));
    CHECK(reader.value().read("empty.txt").value().empty());
    const Result<std::vector<std::byte>> missing = reader.value().read("models/crate.glb");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    // A reader can be moved, and keeps working.
    ArchiveReader moved = std::move(reader).value();
    CHECK(moved.read("models/ball.glb").value() == bytes("a ball"));
}

TEST_CASE("An archive that is missing, unfinished or damaged is refused") {
    const TemporaryFile file("dream_archive_damaged.pak");
    CHECK(ArchiveReader::open(file.path()).error().code == ErrorCode::NotFound);

    SUBCASE("unfinished") {
        {
            Result<ArchiveWriter> writer = ArchiveWriter::create(file.path());
            REQUIRE(writer.has_value());
            REQUIRE(writer.value().add("a.txt", bytes("text")).has_value());
        }
        CHECK(ArchiveReader::open(file.path()).error().code == ErrorCode::ParseError);
    }
    SUBCASE("not an archive") {
        write_raw(file.path(), bytes("PK this is a zip file, honestly"));
        CHECK(ArchiveReader::open(file.path()).error().code == ErrorCode::ParseError);
    }
    SUBCASE("cut short") {
        {
            Result<ArchiveWriter> writer = ArchiveWriter::create(file.path());
            REQUIRE(writer.has_value());
            REQUIRE(writer.value().add("a.txt", bytes("some text")).has_value());
            REQUIRE(writer.value().finish().has_value());
        }
        std::vector<std::byte> data = read_raw(file.path());
        data.resize(data.size() - 3);
        write_raw(file.path(), data);
        CHECK(ArchiveReader::open(file.path()).error().code == ErrorCode::ParseError);
    }
}
