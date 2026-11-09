#include "dream/core/module_registry.h"
#include "dream/platform/platform.h"

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
namespace {

using namespace platform;

// A platform with no window or real storage, for automated runs and machines without a display.
// It runs for `platform.null.frame_limit` frames and then asks to quit. Assets don't exist, and
// user files last only as long as the object.
class NullPlatform final : public Platform {
public:
    explicit NullPlatform(std::int64_t frame_limit) : m_frames_left(frame_limit) {}

    core::Result<void> create_window(const WindowSettings& settings) override {
        m_size = SurfaceSize{settings.width, settings.height};
        return {};
    }

    SurfaceSize surface_size() const override {
        return m_size;
    }

    NativeWindow native_window() const override {
        return {};
    }

    std::optional<Event> poll_event() override {
        // The main loop reads events until there are none, so each "none" is one frame.
        if (m_frames_left <= 0) {
            return QuitRequested{};
        }
        --m_frames_left;
        return std::nullopt;
    }

    Event wait_event() override {
        return QuitRequested{};
    }

    core::Result<std::vector<std::byte>> read_file(FileRoot root, std::string_view path) override {
        if (root == FileRoot::User) {
            const auto file = m_user_files.find(path);
            if (file != m_user_files.end()) {
                return file->second;
            }
        }
        return core::Error{core::ErrorCode::NotFound, std::format("No such file: {}", path)};
    }

    core::Result<void> write_user_file(std::string_view path,
                                       std::span<const std::byte> data) override {
        m_user_files.insert_or_assign(std::string(path),
                                      std::vector<std::byte>(data.begin(), data.end()));
        return {};
    }

private:
    SurfaceSize m_size;
    std::int64_t m_frames_left;
    std::map<std::string, std::vector<std::byte>, std::less<>> m_user_files;
};

core::Result<std::unique_ptr<Platform>> create(const core::Config& config) {
    const std::int64_t frame_limit = config.get_int("platform.null.frame_limit").value_or(1);
    return std::unique_ptr<Platform>{std::make_unique<NullPlatform>(frame_limit)};
}

} // namespace

[[nodiscard]] core::Result<void> register_platform_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Platform>{.name = "null", .create = &create});
}

} // namespace dream::modules
