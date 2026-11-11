#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "mixer/mixer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

using namespace mixer;

namespace {

constexpr unsigned k_rate = 48000;

// A constant level, which makes mixed output easy to predict.
std::shared_ptr<const Sound> constant(float level, std::size_t frames, unsigned channels = 1,
                                      unsigned rate = k_rate) {
    auto sound = std::make_shared<Sound>();
    sound->samples.assign(frames * channels, level);
    sound->channels = channels;
    sound->sample_rate = rate;
    return sound;
}

struct Output {
    std::vector<float> samples;

    [[nodiscard]] float left(std::size_t frame) const {
        return samples[frame * 2];
    }
    [[nodiscard]] float right(std::size_t frame) const {
        return samples[frame * 2 + 1];
    }
    [[nodiscard]] std::size_t sounding_frames() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < samples.size() / 2; ++i) {
            count += (left(i) != 0.0f || right(i) != 0.0f) ? 1 : 0;
        }
        return count;
    }
};

Output mix(Mixer& mixer, std::size_t frames) {
    Output output{std::vector<float>(frames * 2, 9.0f)};
    mixer.mix(output.samples);
    return output;
}

// A stream that counts up, to check the order frames come out in.
class Ramp final : public Stream {
public:
    explicit Ramp(std::size_t length) : m_length(length) {}
    unsigned channels() const override {
        return 1;
    }
    unsigned sample_rate() const override {
        return k_rate;
    }
    std::size_t read(std::span<float> out) override {
        std::size_t count = 0;
        while (count < out.size() && m_next < m_length) {
            out[count++] = static_cast<float>(m_next++) / 100000.0f;
        }
        return count;
    }
    bool rewind() override {
        m_next = 0;
        ++rewinds;
        return true;
    }
    int rewinds = 0;

private:
    std::size_t m_length;
    std::size_t m_next = 0;
};

} // namespace

TEST_CASE("Mixer outputs silence without voices") {
    Mixer mixer(k_rate, 2, 8);
    const Output output = mix(mixer, 64);
    CHECK(output.sounding_frames() == 0);
}

TEST_CASE("Mixer plays a sound once, then the voice finishes and is released") {
    Mixer mixer(k_rate, 1, 8);
    const auto voice = mixer.play(make_sound_stream(constant(0.5f, 1000)), VoiceSettings{});
    REQUIRE(voice.has_value());
    CHECK(mixer.is_playing(*voice));
    CHECK(mixer.voice_count() == 1);

    const Output output = mix(mixer, 1500);
    CHECK(output.left(0) == doctest::Approx(0.5f));
    CHECK(output.right(999) == doctest::Approx(0.5f));
    CHECK(output.sounding_frames() == 1000);
    CHECK_FALSE(mixer.is_playing(*voice));
    CHECK(mixer.voice_count() == 1);
    CHECK(mixer.release_finished() == 1);
    CHECK(mixer.voice_count() == 0);
}

TEST_CASE("Mixer reads streams in order across its decoding window") {
    Mixer mixer(k_rate, 1, 8);
    REQUIRE(mixer.play(std::make_unique<Ramp>(10000), VoiceSettings{}).has_value());
    const Output output = mix(mixer, 10000);
    for (std::size_t frame : {0u, 1u, 4095u, 4096u, 4097u, 9999u}) {
        CHECK(output.left(frame) == doctest::Approx(static_cast<float>(frame) / 100000.0f));
    }
}

TEST_CASE("Mixer loops voices that ask for it") {
    Mixer mixer(k_rate, 1, 8);
    auto ramp = std::make_unique<Ramp>(300);
    Ramp& source = *ramp;
    const auto voice = mixer.play(std::move(ramp), VoiceSettings{.looping = true});
    REQUIRE(voice.has_value());
    const Output output = mix(mixer, 1000);
    CHECK(mixer.is_playing(*voice));
    CHECK(output.left(300) == doctest::Approx(0.0f));
    CHECK(output.left(301) == doctest::Approx(1.0f / 100000.0f));
    CHECK(source.rewinds > 2);
}

TEST_CASE("Mixer resamples by sample rate and pitch") {
    Mixer mixer(k_rate, 1, 8);
    // Half the mixer's rate lasts twice as long, and double pitch undoes that.
    REQUIRE(mixer.play(make_sound_stream(constant(0.5f, 1000, 1, k_rate / 2)), VoiceSettings{})
                .has_value());
    CHECK(mix(mixer, 4000).sounding_frames() == doctest::Approx(2000).epsilon(0.01));

    mixer.release_finished();
    REQUIRE(mixer
                .play(make_sound_stream(constant(0.5f, 1000, 1, k_rate / 2)),
                      VoiceSettings{.pitch = 2.0f})
                .has_value());
    CHECK(mix(mixer, 4000).sounding_frames() == doctest::Approx(1000).epsilon(0.01));
}

TEST_CASE("Mixer applies volume, bus volume and pan") {
    Mixer mixer(k_rate, 2, 8);
    const auto voice = mixer.play(make_sound_stream(constant(0.5f, 100000)),
                                  VoiceSettings{.bus = 1, .volume = 0.5f});
    REQUIRE(voice.has_value());
    CHECK(mix(mixer, 10).left(5) == doctest::Approx(0.25f));

    mixer.set_bus_volume(1, 0.5f);
    CHECK(mix(mixer, 10).right(5) == doctest::Approx(0.125f));
    mixer.set_bus_volume(1, -3.0f);
    CHECK(mixer.bus_volume(1) == doctest::Approx(0.0f));
    CHECK(mix(mixer, 10).sounding_frames() == 0);
    mixer.set_bus_volume(1, 1.0f);

    mixer.stop(*voice);
    const auto panned =
        mixer.play(make_sound_stream(constant(0.5f, 100000)), VoiceSettings{.pan = 1.0f});
    REQUIRE(panned.has_value());
    const Output output = mix(mixer, 10);
    CHECK(output.left(5) == doctest::Approx(0.0f));
    CHECK(output.right(5) == doctest::Approx(0.5f));
}

TEST_CASE("Mixer keeps stereo channels apart") {
    Mixer mixer(k_rate, 1, 8);
    auto sound = std::make_shared<Sound>();
    sound->channels = 2;
    sound->sample_rate = k_rate;
    sound->samples = {0.1f, -0.2f, 0.1f, -0.2f, 0.1f, -0.2f};
    REQUIRE(mixer.play(make_sound_stream(sound), VoiceSettings{}).has_value());
    const Output output = mix(mixer, 3);
    CHECK(output.left(1) == doctest::Approx(0.1f));
    CHECK(output.right(1) == doctest::Approx(-0.2f));
}

TEST_CASE("Mixer makes positioned voices quieter with distance and pans them") {
    Mixer mixer(k_rate, 1, 8);
    mixer.set_listener(Vec3{}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const auto voice = mixer.play(make_sound_stream(constant(0.5f, 100000)),
                                  VoiceSettings{.position = Vec3{0.0f, 0.0f, -1.0f},
                                                .min_distance = 1.0f,
                                                .max_distance = 100.0f});
    REQUIRE(voice.has_value());
    CHECK(mix(mixer, 10).left(5) == doctest::Approx(0.5f));

    mixer.set_position(*voice, Vec3{0.0f, 0.0f, -20.0f});
    CHECK(mix(mixer, 10).left(5) == doctest::Approx(0.5f / 20.0f));

    // To the right: the left side is quieter, and beyond max_distance it fades no further.
    mixer.set_position(*voice, Vec3{500.0f, 0.0f, 0.0f});
    const Output right = mix(mixer, 10);
    CHECK(right.right(5) == doctest::Approx(0.5f / 100.0f));
    CHECK(right.left(5) < right.right(5) * 0.5f);
}

TEST_CASE("Mixer refuses what it can't play") {
    Mixer mixer(k_rate, 1, 1);
    CHECK_FALSE(mixer.play(nullptr, VoiceSettings{}).has_value());
    CHECK_FALSE(mixer.play(make_sound_stream(constant(0.5f, 0)), VoiceSettings{}).has_value());
    CHECK_FALSE(
        mixer.play(make_sound_stream(constant(0.5f, 10)), VoiceSettings{.bus = 1}).has_value());
    const auto voice = mixer.play(make_sound_stream(constant(0.5f, 10)), VoiceSettings{});
    REQUIRE(voice.has_value());
    CHECK_FALSE(mixer.play(make_sound_stream(constant(0.5f, 10)), VoiceSettings{}).has_value());

    // A stopped voice's id is not reused by the next voice.
    mixer.stop(*voice);
    mixer.stop(*voice);
    const auto next = mixer.play(make_sound_stream(constant(0.5f, 10)), VoiceSettings{});
    REQUIRE(next.has_value());
    CHECK_FALSE(*next == *voice);
    CHECK_FALSE(mixer.is_playing(*voice));
}

TEST_CASE("Mixer limits its output") {
    Mixer mixer(k_rate, 1, 8);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(mixer.play(make_sound_stream(constant(0.5f, 100)), VoiceSettings{}).has_value());
    }
    CHECK(mix(mixer, 10).left(5) == doctest::Approx(1.0f));
}
