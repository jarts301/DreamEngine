// No gamepads, ever, for projects and platforms without them.

#include "dream/core/module_registry.h"
#include "dream/input_device/input_device.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace input_device;

class NullInputDevice final : public InputDevice {
public:
    void update() override {}

    std::span<const GamepadState> gamepads() const override {
        return {};
    }

    bool rumble(std::uint32_t /*id*/, float /*low*/, float /*high*/,
                std::uint32_t /*milliseconds*/) override {
        return false;
    }
};

core::Result<std::unique_ptr<InputDevice>> create(const core::Config& /*config*/) {
    return std::unique_ptr<InputDevice>{std::make_unique<NullInputDevice>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_input_device_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<InputDevice>{.name = "null", .create = &create});
}

} // namespace dream::modules
