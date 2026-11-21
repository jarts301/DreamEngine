// Makes a model's levels of detail at build time (ADR 0042): reads a .glb as the runtime does,
// gives it tangents if the game's platform lists a tangent generator, makes its simpler versions
// with meshoptimizer, and writes them to a .lods file that the runtime reads in place of making
// them as the model loads.
//
//     dream_model_cook <model.glb> <model.lods> <dream.toml> <windows|android|linux>
//
// The game's dream.toml gives the [lods] settings and the platform's module lists. With
// `[lods] cook = false`, or `levels = 0`, it writes an empty file, which the runtime ignores.

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"
#include "dream/runtime/model.h"
#include "dream/tangent_generator/tangent_generator.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_mesh_simplifier_meshoptimizer(
    core::ModuleRegistry& registry);
[[nodiscard]] core::Result<void> register_tangent_generator_mikktspace(
    core::ModuleRegistry& registry);
[[nodiscard]] core::Result<void> register_tangent_generator_null(core::ModuleRegistry& registry);
} // namespace dream::modules

using namespace dream;

namespace {

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t index = 0; index < chars.size(); ++index) {
        bytes[index] = static_cast<std::byte>(chars[index]);
    }
    return bytes;
}

bool write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

int fail(std::string_view model, std::string_view message) {
    std::fprintf(stderr, "dream_model_cook: %.*s: %.*s\n", static_cast<int>(model.size()),
                 model.data(), static_cast<int>(message.size()), message.data());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "Usage: dream_model_cook <model.glb> <model.lods> <dream.toml> "
                             "<windows|android|linux>\n");
        return 2;
    }
    const std::filesystem::path model_path = argv[1];
    const std::filesystem::path output_path = argv[2];
    const std::string model_name = model_path.filename().string();
    const std::string platform = argv[4];

    const std::optional<std::vector<std::byte>> config_file = read_file(argv[3]);
    if (!config_file.has_value()) {
        return fail(model_name, "the game's configuration can't be read");
    }
    const std::string config_text(reinterpret_cast<const char*>(config_file->data()),
                                  config_file->size());
    const core::Result<core::Config> config = core::Config::parse(config_text);
    if (!config.has_value()) {
        return fail(model_name, config.error().message);
    }
    const core::Result<runtime::LodOptions> options = runtime::read_lod_options(config.value());
    if (!options.has_value()) {
        return fail(model_name, options.error().message);
    }
    // Nothing to cook: an empty file, which the runtime ignores.
    if (!options.value().cook || options.value().levels == 0) {
        return write_file(output_path, {}) ? 0 : fail(model_name, "can't write the output");
    }

    const std::optional<std::vector<std::byte>> file = read_file(model_path);
    if (!file.has_value()) {
        return fail(model_name, "can't be read");
    }
    // Images beside the model, as the runtime reads them. They don't change the geometry, but the
    // model is read the same way, so it fails or succeeds as it would in the game.
    const std::filesystem::path folder = model_path.parent_path();
    const runtime::ModelFileReader read_image =
        [&folder](std::string_view relative) -> core::Result<std::vector<std::byte>> {
        std::optional<std::vector<std::byte>> image = read_file(folder / std::string(relative));
        if (!image.has_value()) {
            return core::Error{core::ErrorCode::NotFound,
                               std::format("'{}' does not exist", relative)};
        }
        return std::move(image).value();
    };
    core::Result<runtime::ModelData> data = runtime::parse_glb(file.value(), read_image);
    if (!data.has_value()) {
        return fail(model_name, data.error().message);
    }

    core::ModuleRegistry registry;
    if (!modules::register_mesh_simplifier_meshoptimizer(registry).has_value() ||
        !modules::register_tangent_generator_mikktspace(registry).has_value() ||
        !modules::register_tangent_generator_null(registry).has_value()) {
        return fail(model_name, "the modules can't be registered");
    }

    // Tangents first, exactly as the game would make them on this platform, since they can add
    // vertices that the simpler versions then use.
    const std::string tangent_key = std::format("modules.{}.tangent_generator", platform);
    if (const auto listed = config.value().get_string_list(tangent_key);
        listed.has_value() && !listed->empty()) {
        const std::vector<std::string> preference(listed->begin(), listed->end());
        auto tangents =
            registry.create<tangent_generator::TangentGenerator>(preference, config.value());
        if (!tangents.has_value()) {
            return fail(model_name, tangents.error().message);
        }
        if (const core::Result<void> made =
                runtime::generate_tangents(data.value(), *tangents.value().instance);
            !made.has_value()) {
            return fail(model_name, made.error().message);
        }
    }

    const std::vector<std::string> simplifier_name{"meshoptimizer"};
    auto simplifier =
        registry.create<mesh_simplifier::MeshSimplifier>(simplifier_name, config.value());
    if (!simplifier.has_value()) {
        return fail(model_name, simplifier.error().message);
    }
    if (const core::Result<void> made = runtime::generate_lods(
            data.value(), *simplifier.value().instance, options.value());
        !made.has_value()) {
        return fail(model_name, made.error().message);
    }

    const std::vector<std::byte> cooked =
        runtime::write_cooked_lods(data.value(), file.value(), options.value());
    if (!write_file(output_path, cooked)) {
        return fail(model_name, "can't write the output");
    }
    return 0;
}
