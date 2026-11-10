#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_audio_null(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::audio::AudioDevice> make_device() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_audio_null(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<audio::AudioDevice>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

TEST_CASE("Null audio finishes voices that don't loop at the next update") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(0.1f), false).value();
    const VoiceHandle voice = device->play(sound, PlayDesc{}).value();
    device->update();
    CHECK_FALSE(device->is_playing(voice));
}
