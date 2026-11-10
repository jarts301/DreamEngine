#include "dream/runtime/audio.h"

#include "dream/runtime/app.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

// Records what the runtime asks of an audio device.
struct FakeAudioState {
    std::map<std::uint32_t, bool> sounds; // Index to whether it streams.
    std::map<std::uint32_t, audio::PlayDesc> voices;
    std::vector<core::Vec3> positions;
    core::Vec3 listener;
    float music_volume = 1.0f;
    int suspends = 0;
    int resumes = 0;
    int updates = 0;
};

class FakeAudio final : public audio::AudioDevice {
public:
    explicit FakeAudio(FakeAudioState& state) : m_state(state) {}

    core::Result<audio::SoundHandle> create_sound(std::span<const std::byte> file,
                                                  bool stream) override {
        if (file.empty()) {
            return core::Error{core::ErrorCode::InvalidArgument, "empty"};
        }
        m_state.sounds[++m_next] = stream;
        return audio::SoundHandle{.index = m_next, .generation = 1};
    }
    void destroy_sound(audio::SoundHandle sound) override {
        m_state.sounds.erase(sound.index);
    }
    std::optional<float> sound_length(audio::SoundHandle) const override {
        return std::nullopt;
    }
    core::Result<audio::VoiceHandle> play(audio::SoundHandle sound,
                                          const audio::PlayDesc& desc) override {
        if (!m_state.sounds.contains(sound.index)) {
            return core::Error{core::ErrorCode::InvalidArgument, "no sound"};
        }
        m_state.voices[++m_next] = desc;
        return audio::VoiceHandle{.index = m_next, .generation = 1};
    }
    void stop(audio::VoiceHandle voice) override {
        m_state.voices.erase(voice.index);
    }
    bool is_playing(audio::VoiceHandle voice) const override {
        return m_state.voices.contains(voice.index);
    }
    void set_volume(audio::VoiceHandle, float) override {}
    void set_pitch(audio::VoiceHandle, float) override {}
    void set_position(audio::VoiceHandle, core::Vec3 position) override {
        m_state.positions.push_back(position);
    }
    void set_listener(core::Vec3 position, core::Vec3, core::Vec3) override {
        m_state.listener = position;
    }
    void set_bus_volume(audio::Bus bus, float volume) override {
        if (bus == audio::Bus::Music) {
            m_state.music_volume = volume;
        }
    }
    float bus_volume(audio::Bus bus) const override {
        return bus == audio::Bus::Music ? m_state.music_volume : 1.0f;
    }
    void suspend() override {
        ++m_state.suspends;
    }
    void resume() override {
        ++m_state.resumes;
    }
    void update() override {
        ++m_state.updates;
    }
    std::size_t voice_count() const override {
        return m_state.voices.size();
    }

private:
    FakeAudioState& m_state;
    std::uint32_t m_next = 0;
};

const core::Config k_config = core::Config::parse("[audio]\nmusic_volume = 0.4").value();

struct AudioFixture {
    FakeState platform;
    FakeAudioState state;
    App app{std::make_unique<FakePlatform>(platform), k_config};
    Audio audio;

    AudioFixture() {
        platform.assets["sounds/beep.wav"] = std::vector<std::byte>(100);
        platform.assets["sounds/song.wav"] = std::vector<std::byte>(300 * 1024);
        REQUIRE(audio.initialize(app, std::make_unique<FakeAudio>(state)).has_value());
    }
};

} // namespace

TEST_CASE_FIXTURE(AudioFixture, "Audio reads bus volumes and streams only large files") {
    CHECK(state.music_volume == doctest::Approx(0.4f));
    audio.set_bus_volume(audio::Bus::Music, 0.7f);
    CHECK(audio.bus_volume(audio::Bus::Music) == doctest::Approx(0.7f));

    const auto beep = app.assets().load<SoundAsset>("assets:/sounds/beep.wav");
    const auto song = app.assets().load<SoundAsset>("assets:/sounds/song.wav");
    REQUIRE(beep.has_value());
    REQUIRE(song.has_value());
    CHECK_FALSE(state.sounds.at(beep.value()->get().index));
    CHECK(state.sounds.at(song.value()->get().index));
}

TEST_CASE_FIXTURE(AudioFixture, "Audio plays one-off sounds, positioned or not") {
    const auto beep = app.assets().load<SoundAsset>("assets:/sounds/beep.wav").value();
    const audio::VoiceHandle plain = audio.play(beep, audio::PlayDesc{.volume = 0.5f});
    const audio::VoiceHandle placed = audio.play_at(beep, core::Vec3{1.0f, 2.0f, 3.0f});
    REQUIRE(plain.is_valid());
    CHECK(state.voices.at(plain.index).volume == doctest::Approx(0.5f));
    CHECK(state.voices.at(placed.index).position->z == doctest::Approx(3.0f));

    audio.stop(plain);
    CHECK_FALSE(state.voices.contains(plain.index));
    CHECK_FALSE(audio.play(nullptr).is_valid());
}

TEST_CASE_FIXTURE(AudioFixture, "Audio starts scene sources once, follows them and stops them") {
    const std::string_view scene = R"(
[entities.ear.transform]
position = [0.0, 1.0, 5.0]
[entities.ear.audio_listener]

[entities.radio.transform]
position = [4.0, 0.0, 0.0]
[entities.radio.audio_source]
sound = "assets:/sounds/song.wav"
bus = "music"
loop = true
spatial = true
volume = 0.8

[entities.quiet.audio_source]
sound = "assets:/sounds/beep.wav"
autoplay = false
)";
    core::Result<std::unique_ptr<World>> loaded = app.scenes().parse(scene);
    REQUIRE(loaded.has_value());
    World& world = *loaded.value();

    audio.update(&world);
    CHECK(state.listener.z == doctest::Approx(5.0f));
    REQUIRE(state.voices.size() == 1);
    const audio::VoiceHandle voice = audio.source_voice(world.find("radio"));
    const audio::PlayDesc& desc = state.voices.at(voice.index);
    CHECK(desc.bus == audio::Bus::Music);
    CHECK(desc.looping);
    CHECK(desc.volume == doctest::Approx(0.8f));
    CHECK(desc.position->x == doctest::Approx(4.0f));
    CHECK_FALSE(audio.source_voice(world.find("quiet")).is_valid());

    // It follows its entity, and isn't started twice.
    world.registry().get<Transform>(world.find("radio")).position.x = 6.0f;
    world.update_transforms();
    audio.update(&world);
    CHECK(state.voices.size() == 1);
    CHECK(state.positions.back().x == doctest::Approx(6.0f));

    world.destroy_entity(world.find("radio"));
    audio.update(&world);
    CHECK(state.voices.empty());
    CHECK(state.updates == 3);

    // Another world stops the first one's sources.
    REQUIRE(state.voices.empty());
    audio.update(nullptr);
    CHECK(state.updates == 4);
}

TEST_CASE_FIXTURE(AudioFixture, "Audio goes quiet in the background") {
    app.events().publish(platform::AppPaused{});
    app.events().publish(platform::AppResumed{});
    CHECK(state.suspends == 1);
    CHECK(state.resumes == 1);
}

TEST_CASE_FIXTURE(AudioFixture, "Audio reports mistakes in scene files") {
    for (const char* scene : {
             "[entities.a.audio_source]\nbus = \"music\"",
             "[entities.a.audio_source]\nsound = \"assets:/sounds/beep.wav\"\nbus = \"voice\"",
             "[entities.a.audio_source]\nsound = \"assets:/sounds/missing.wav\"",
             "[entities.a.audio_source]\nsound = \"assets:/sounds/beep.wav\"\nlooping = true",
             "[entities.a.audio_listener]\nvolume = 1.0",
         }) {
        CAPTURE(scene);
        CHECK_FALSE(app.scenes().parse(scene).has_value());
    }
}
