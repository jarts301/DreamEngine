#pragma once

// A ui::Host for interface backend tests: files come from memory, and textures are only counted.

#include "dream/ui/draw_list.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tests {

class FakeHost final : public dream::ui::Host {
public:
    dream::core::Result<std::vector<std::byte>> read_file(std::string_view path) override {
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return dream::core::Error{dream::core::ErrorCode::NotFound,
                                      "No file " + std::string(path)};
        }
        const std::string& text = found->second;
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return std::vector<std::byte>(bytes, bytes + text.size());
    }

    dream::core::Result<dream::ui::TextureId> create_texture(std::uint32_t width,
                                                             std::uint32_t height,
                                                             std::span<const std::byte> rgba,
                                                             dream::ui::TextureKind) override {
        if (rgba.size() != std::size_t{width} * height * 4) {
            return dream::core::Error{dream::core::ErrorCode::InvalidArgument, "Bad pixels"};
        }
        live.insert(++next);
        return next;
    }

    dream::core::Result<dream::ui::LoadedTexture> load_texture(std::string_view path,
                                                               bool) override {
        if (!files.contains(std::string(path))) {
            return dream::core::Error{dream::core::ErrorCode::NotFound, "No image"};
        }
        live.insert(++next);
        return dream::ui::LoadedTexture{next, 8, 8};
    }

    void release_texture(dream::ui::TextureId texture) override {
        live.erase(texture);
    }

    // Adds a file from disk, such as a font.
    void add_disk_file(const std::string& virtual_path, const std::string& disk_path) {
        std::ifstream stream(disk_path, std::ios::binary);
        files[virtual_path] = std::string(std::istreambuf_iterator<char>(stream), {});
    }

    std::map<std::string, std::string> files;
    std::set<dream::ui::TextureId> live;
    dream::ui::TextureId next = 0;
};

} // namespace tests
