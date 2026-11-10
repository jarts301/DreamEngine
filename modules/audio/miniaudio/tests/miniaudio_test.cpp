#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_audio_miniaudio(core::ModuleRegistry& registry);
std::uint64_t miniaudio_read_frames(audio::AudioDevice& device, std::span<float> interleaved_stereo);
}

// Without sound hardware, mixed only when read, so the tests control time.
std::unique_ptr<dream::audio::AudioDevice> make_device() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_audio_miniaudio(registry).has_value());
    const core::Config config = core::Config::parse("[audio.miniaudio]\ndevice = false").value();
    const std::vector<std::string> preference{"miniaudio"};
    auto created = registry.create<audio::AudioDevice>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

namespace {

// The loudest sample in the next `seconds` of mixed output, at 48 kHz.
float peak(dream::audio::AudioDevice& device, float seconds) {
    std::vector<float> samples(static_cast<std::size_t>(seconds * 48000.0f) * 2);
    const std::uint64_t read = dream::modules::miniaudio_read_frames(device, samples);
    REQUIRE(read == samples.size() / 2);
    float loudest = 0.0f;
    for (const float sample : samples) {
        loudest = std::max(loudest, std::abs(sample));
    }
    return loudest;
}

} // namespace

TEST_CASE("miniaudio decodes WAV files and knows their length") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(0.5f), false).value();
    CHECK(*device->sound_length(sound) == doctest::Approx(0.5f).epsilon(0.01));

    const std::vector<std::byte> garbage(256, std::byte{0x5a});
    CHECK_FALSE(device->create_sound(garbage, false).has_value());
}

TEST_CASE("miniaudio mixes playing voices, and finished ones go away") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(0.25f), false).value();

    CHECK(peak(*device, 0.05f) == doctest::Approx(0.0f));
    const VoiceHandle voice = device->play(sound, PlayDesc{}).value();
    CHECK(peak(*device, 0.1f) > 0.2f);

    // Past the end of the sound it is silent, and the next update releases it.
    CHECK(peak(*device, 0.3f) > 0.0f);
    CHECK(peak(*device, 0.05f) == doctest::Approx(0.0f));
    CHECK_FALSE(device->is_playing(voice));
    device->update();
    CHECK(device->voice_count() == 0);
}

TEST_CASE("miniaudio scales voices by their volume and bus") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(1.0f), true).value();

    const VoiceHandle voice = device->play(sound, PlayDesc{.bus = Bus::Music, .looping = true}).value();
    const float full = peak(*device, 0.1f);
    CHECK(full > 0.2f);

    device->set_bus_volume(Bus::Music, 0.5f);
    peak(*device, 0.05f);
    CHECK(peak(*device, 0.1f) == doctest::Approx(full * 0.5f).epsilon(0.05));

    device->set_volume(voice, 0.0f);
    peak(*device, 0.05f);
    CHECK(peak(*device, 0.1f) < 0.01f);

    // Streamed sounds loop past their end.
    device->set_volume(voice, 1.0f);
    peak(*device, 1.2f);
    CHECK(device->is_playing(voice));
}

TEST_CASE("miniaudio makes far sounds quieter") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(1.0f), false).value();
    device->set_listener(dream::core::Vec3{}, dream::core::Vec3{0.0f, 0.0f, -1.0f},
                         dream::core::Vec3{0.0f, 1.0f, 0.0f});

    const VoiceHandle near_voice =
        device->play(sound, PlayDesc{.looping = true, .position = dream::core::Vec3{0.0f, 0.0f, -1.0f}})
            .value();
    peak(*device, 0.05f);
    const float near_peak = peak(*device, 0.1f);
    device->set_position(near_voice, dream::core::Vec3{0.0f, 0.0f, -20.0f});
    peak(*device, 0.1f);
    CHECK(peak(*device, 0.1f) < near_peak * 0.25f);
}
