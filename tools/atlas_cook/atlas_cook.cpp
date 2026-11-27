// Packs every PNG in a folder into one image, an atlas, at build time (ADR 0052), so the sprites
// cut from it are drawn from one texture, together:
//
//     dream_atlas_cook <folder> <atlas.png> <atlas.atlas> <padding> <max_size>
//
// Each image becomes a region named by its path in the folder, without ".png", such as "hero" or
// "trees/oak". The .atlas file is TOML that the runtime reads:
//
//     image = "things.png"      # Beside the .atlas file.
//     width = 128
//     height = 64
//
//     [regions]
//     "hero" = [2, 2, 32, 16]   # x, y, width and height, in pixels from the top left.
//
// Around each image, `padding` pixels repeat its edge, so filtering near an edge never reads a
// neighbor. The atlas is the smallest power-of-two size, up to `max_size` on a side, that holds
// them all. The same images always give the same files.
//
// Only the C++ standard library and stb, so vcpkg can build it quickly for the build machine.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_RECT_PACK_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_rect_pack.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Image {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels; // RGBA, row by row from the top.
    int x = 0;                         // Where it is placed in the atlas, padding left out.
    int y = 0;
};

int fail(const std::string& message) {
    std::fprintf(stderr, "dream_atlas_cook: %s\n", message.c_str());
    return 1;
}

bool read_number(std::string_view text, int& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

// A region's name is quoted in TOML and becomes part of a dotted key in the runtime, so it keeps
// to letters, digits, '_', '-' and '/' between folders.
bool good_name(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '/';
    });
}

// Tries to place every image, with its padding, in an atlas of `width` by `height`.
bool pack(std::vector<Image>& images, int padding, int width, int height) {
    std::vector<stbrp_node> nodes(static_cast<std::size_t>(width));
    stbrp_context context;
    stbrp_init_target(&context, width, height, nodes.data(), static_cast<int>(nodes.size()));
    std::vector<stbrp_rect> rects(images.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        rects[index].id = static_cast<int>(index);
        rects[index].w = images[index].width + 2 * padding;
        rects[index].h = images[index].height + 2 * padding;
    }
    if (stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size())) != 1) {
        return false;
    }
    for (const stbrp_rect& rect : rects) {
        Image& image = images[static_cast<std::size_t>(rect.id)];
        image.x = rect.x + padding;
        image.y = rect.y + padding;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        return fail("usage: dream_atlas_cook <folder> <atlas.png> <atlas.atlas> <padding> "
                    "<max_size>");
    }
    const fs::path folder = argv[1];
    const fs::path output_image = argv[2];
    const fs::path output_atlas = argv[3];
    int padding = 0;
    int max_size = 0;
    if (!read_number(argv[4], padding) || padding < 0 || padding > 16) {
        return fail("padding must be a whole number from 0 to 16");
    }
    if (!read_number(argv[5], max_size) || max_size < 16 || max_size > 8192) {
        return fail("max_size must be a whole number from 16 to 8192");
    }

    // Every PNG under the folder, in the order of their names, so each build packs alike.
    std::error_code error;
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".png") {
            paths.push_back(it->path());
        }
    }
    if (error) {
        return fail("can't read the folder " + folder.string() + ": " + error.message());
    }
    std::ranges::sort(paths);
    if (paths.empty()) {
        return fail("the folder " + folder.string() + " has no PNG images");
    }

    std::vector<Image> images;
    long long area = 0;
    for (const fs::path& path : paths) {
        Image image;
        image.name = path.lexically_relative(folder).replace_extension().generic_string();
        if (!good_name(image.name)) {
            return fail("\"" + path.string() + "\" can't name a region: use letters, digits, "
                        "'_' and '-'");
        }
        int channels = 0;
        unsigned char* pixels =
            stbi_load(path.string().c_str(), &image.width, &image.height, &channels, 4);
        if (pixels == nullptr) {
            return fail("can't read " + path.string() + ": " + stbi_failure_reason());
        }
        image.pixels.assign(pixels, pixels + std::size_t{4} * static_cast<std::size_t>(image.width) *
                                                 static_cast<std::size_t>(image.height));
        stbi_image_free(pixels);
        area += static_cast<long long>(image.width + 2 * padding) * (image.height + 2 * padding);
        images.push_back(std::move(image));
    }

    // The smallest power-of-two size that holds them, wide before tall: 64x32, 64x64, 128x64...
    int width = 16;
    int height = 16;
    bool packed = false;
    while (width <= max_size) {
        if (static_cast<long long>(width) * height >= area &&
            pack(images, padding, width, height)) {
            packed = true;
            break;
        }
        if (height < width) {
            height *= 2;
        } else {
            width *= 2;
        }
    }
    if (!packed) {
        return fail("the images in " + folder.string() + " don't fit in " +
                    std::to_string(max_size) + " by " + std::to_string(max_size) +
                    " pixels: raise max_size, or split them into two atlases");
    }

    // Each image, and around it its edge pixels repeated over the padding.
    std::vector<unsigned char> atlas(std::size_t{4} * static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height));
    for (const Image& image : images) {
        for (int y = -padding; y < image.height + padding; ++y) {
            const int from_y = std::clamp(y, 0, image.height - 1);
            for (int x = -padding; x < image.width + padding; ++x) {
                const int from_x = std::clamp(x, 0, image.width - 1);
                const unsigned char* from =
                    &image.pixels[std::size_t{4} * static_cast<std::size_t>(from_y * image.width + from_x)];
                unsigned char* to = &atlas[std::size_t{4} * static_cast<std::size_t>(
                                                                (image.y + y) * width + image.x + x)];
                std::copy(from, from + 4, to);
            }
        }
    }
    if (stbi_write_png(output_image.string().c_str(), width, height, 4, atlas.data(), width * 4) ==
        0) {
        return fail("can't write " + output_image.string());
    }

    std::ofstream out(output_atlas, std::ios::binary);
    out << "# Made by dream_atlas_cook from " << images.size() << " images; the build makes it "
        << "again when they change.\n"
        << "image = \"" << output_image.filename().generic_string() << "\"\n"
        << "width = " << width << "\nheight = " << height << "\n\n[regions]\n";
    for (const Image& image : images) {
        out << '"' << image.name << "\" = [" << image.x << ", " << image.y << ", " << image.width
            << ", " << image.height << "]\n";
    }
    if (!out) {
        return fail("can't write " + output_atlas.string());
    }
    std::printf("%s: %zu images in %d x %d\n", output_atlas.filename().string().c_str(),
                images.size(), width, height);
    return 0;
}
