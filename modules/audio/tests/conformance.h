#pragma once

// Tests that every audio backend must pass. A backend's test program defines make_device() and
// includes this file once. They don't depend on time passing, since some backends mix only when
// asked.

#include "dream/audio/audio.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

[[nodiscard]] std::unique_ptr<dream::audio::AudioDevice> make_device();

namespace conformance {

using namespace dream;
using namespace dream::audio;

// A mono 16-bit WAV file holding a sine tone.
inline std::vector<std::byte> make_wav(float seconds, float frequency = 440.0f,
                                       std::uint32_t rate = 22050) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<std::byte> file;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        file.insert(file.end(), bytes, bytes + size);
    };
    const auto put32 = [&](std::uint32_t value) { put(&value, 4); };
    const auto put16 = [&](std::uint16_t value) { put(&value, 2); };

    put("RIFF", 4);
    put32(36 + frames * 2);
    put("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(1); // Mono
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    put("data", 4);
    put32(frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const auto sample = static_cast<std::int16_t>(std::sin(t * frequency * 6.2831853f) * 12000.0f);
        put16(static_cast<std::uint16_t>(sample));
    }
    return file;
}

} // namespace conformance

TEST_CASE("Audio rejects empty files and unknown sounds or buses") {
    using namespace conformance;
    const auto device = make_device();
    CHECK_FALSE(device->create_sound({}, false).has_value());
    CHECK_FALSE(device->play(SoundHandle{}, PlayDesc{}).has_value());

    const SoundHandle sound = device->create_sound(make_wav(0.2f), false).value();
    CHECK_FALSE(device->play(sound, PlayDesc{.bus = Bus::Count}).has_value());
}

TEST_CASE("Audio voices play until stopped, and stale handles are ignored") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(0.5f), false).value();

    const VoiceHandle voice = device->play(sound, PlayDesc{.volume = 0.5f}).value();
    CHECK(device->is_playing(voice));
    CHECK(device->voice_count() == 1);

    device->set_volume(voice, 0.2f);
    device->set_pitch(voice, 1.5f);
    device->stop(voice);
    device->stop(voice);
    CHECK_FALSE(device->is_playing(voice));
    CHECK(device->voice_count() == 0);

    // Calls with a stale handle do nothing.
    device->set_volume(voice, 1.0f);
    device->set_position(voice, core::Vec3{});
}

TEST_CASE("Audio looping voices survive updates, and destroying a sound stops them") {
    using namespace conformance;
    const auto device = make_device();
    const SoundHandle sound = device->create_sound(make_wav(0.3f), true).value();

    const VoiceHandle music = device->play(sound, PlayDesc{.bus = Bus::Music, .looping = true}).value();
    const VoiceHandle positioned =
        device
            ->play(sound, PlayDesc{.looping = true, .position = core::Vec3{3.0f, 0.0f, -2.0f}})
            .value();
    device->set_listener(core::Vec3{}, core::Vec3{0.0f, 0.0f, -1.0f}, core::Vec3{0.0f, 1.0f, 0.0f});
    device->set_position(positioned, core::Vec3{1.0f, 0.0f, 0.0f});
    for (int i = 0; i < 3; ++i) {
        device->update();
    }
    CHECK(device->is_playing(music));
    CHECK(device->is_playing(positioned));

    device->destroy_sound(sound);
    device->destroy_sound(sound);
    CHECK_FALSE(device->is_playing(music));
    CHECK(device->voice_count() == 0);
    CHECK_FALSE(device->play(sound, PlayDesc{}).has_value());
}

TEST_CASE("Audio buses keep their volumes") {
    using namespace conformance;
    const auto device = make_device();
    CHECK(device->bus_volume(Bus::Music) == doctest::Approx(1.0f));
    device->set_bus_volume(Bus::Music, 0.25f);
    device->set_bus_volume(Bus::Effects, -1.0f);
    CHECK(device->bus_volume(Bus::Music) == doctest::Approx(0.25f));
    CHECK(device->bus_volume(Bus::Effects) == doctest::Approx(0.0f));
    CHECK(device->bus_volume(Bus::Interface) == doctest::Approx(1.0f));

    device->suspend();
    device->resume();
}
