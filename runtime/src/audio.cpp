#include "dream/runtime/audio.h"

#include "dream/core/log.h"
#include "dream/core/math/matrix.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/scene_loader.h"

#include <format>
#include <string>

namespace dream::runtime {
namespace {

constexpr std::size_t k_stream_threshold_bytes = 256 * 1024;

core::Error scene_error(std::string message) {
    return core::Error{core::ErrorCode::ParseError, std::move(message)};
}

core::Result<void> read_audio_source(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (core::Result<void> known =
            data.check_keys({"sound", "bus", "volume", "pitch", "loop", "autoplay", "spatial",
                             "min_distance", "max_distance"});
        !known.has_value()) {
        return known;
    }

    AudioSource source;
    const core::Result<std::string> path = data.text("sound");
    if (!path.has_value()) {
        return path.error();
    }
    const core::Result<std::string> bus = data.text("bus", "effects");
    if (!bus.has_value()) {
        return bus.error();
    }
    if (bus.value() == "effects") {
        source.desc.bus = audio::Bus::Effects;
    } else if (bus.value() == "music") {
        source.desc.bus = audio::Bus::Music;
    } else if (bus.value() == "interface") {
        source.desc.bus = audio::Bus::Interface;
    } else {
        return scene_error(std::format("unknown bus '{}'", bus.value()));
    }

    const auto number = [&](std::string_view key, float& target) -> core::Result<void> {
        const core::Result<float> value = data.number(key, target);
        if (!value.has_value()) {
            return value.error();
        }
        target = value.value();
        return {};
    };
    const auto flag = [&](std::string_view key, bool& target) -> core::Result<void> {
        const core::Result<bool> value = data.flag(key, target);
        if (!value.has_value()) {
            return value.error();
        }
        target = value.value();
        return {};
    };
    for (const core::Result<void>& read :
         {number("volume", source.desc.volume), number("pitch", source.desc.pitch),
          number("min_distance", source.desc.min_distance),
          number("max_distance", source.desc.max_distance), flag("loop", source.desc.looping),
          flag("autoplay", source.autoplay), flag("spatial", source.spatial)}) {
        if (!read.has_value()) {
            return read;
        }
    }

    core::Result<AssetRef<SoundAsset>> sound = context.assets.load<SoundAsset>(path.value());
    if (!sound.has_value()) {
        return sound.error();
    }
    source.sound = std::move(sound).value();
    context.world.registry().emplace_or_replace<AudioSource>(context.entity, std::move(source));
    return {};
}

core::Result<void> read_audio_listener(ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys({}); !known.has_value()) {
        return known;
    }
    context.world.registry().emplace_or_replace<AudioListener>(context.entity);
    return {};
}

} // namespace

Audio::~Audio() {
    m_subscriptions.clear();
    if (m_device != nullptr) {
        for (const auto& [entity, voice] : m_source_voices) {
            m_device->stop(voice);
        }
    }
}

core::Result<void> Audio::initialize(App& app) {
    core::Result<std::unique_ptr<audio::AudioDevice>> backend =
        app.create_module<audio::AudioDevice>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, std::move(backend).value());
}

core::Result<void> Audio::initialize(App& app, std::unique_ptr<audio::AudioDevice> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "Audio needs a backend"};
    }
    m_device = std::move(backend);

    constexpr std::pair<const char*, audio::Bus> k_buses[] = {
        {"audio.effects_volume", audio::Bus::Effects},
        {"audio.music_volume", audio::Bus::Music},
        {"audio.interface_volume", audio::Bus::Interface},
    };
    for (const auto& [key, bus] : k_buses) {
        if (app.config().contains(key)) {
            const std::optional<double> volume = app.config().get_float(key);
            if (!volume.has_value() || *volume < 0.0) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   std::format("Setting '{}' must be a number of 0 or more", key)};
            }
            m_device->set_bus_volume(bus, static_cast<float>(*volume));
        }
    }

    audio::AudioDevice* const device = m_device.get();
    app.assets().register_loader<SoundAsset>(
        [device](AssetLoadContext& context) -> core::Result<SoundAsset> {
            const auto file = context.files.read(context.path);
            if (!file.has_value()) {
                return file.error();
            }
            const bool stream = file.value().size() > k_stream_threshold_bytes;
            const core::Result<audio::SoundHandle> sound =
                device->create_sound(file.value(), stream);
            if (!sound.has_value()) {
                return core::Error{sound.error().code, std::format("'{}': {}", context.path,
                                                                   sound.error().message)};
            }
            return SoundAsset(*device, sound.value());
        });
    app.scenes().register_component("audio_source", &read_audio_source);
    app.scenes().register_component("audio_listener", &read_audio_listener);

    m_subscriptions.push_back(
        app.events().subscribe<platform::AppPaused>([device](const platform::AppPaused&) {
            device->suspend();
        }));
    m_subscriptions.push_back(
        app.events().subscribe<platform::AppResumed>([device](const platform::AppResumed&) {
            device->resume();
        }));
    return {};
}

void Audio::update(const World* world) {
    if (m_device == nullptr) {
        return;
    }
    DREAM_PROFILE_ZONE("Audio update");

    // Voices started for another world's sources are stopped.
    if (world != m_last_world) {
        for (const auto& [entity, voice] : m_source_voices) {
            m_device->stop(voice);
        }
        m_source_voices.clear();
        m_last_world = world;
    }

    if (world != nullptr) {
        const Registry& registry = world->registry();

        const auto listeners = registry.view<const AudioListener, const WorldTransform>();
        if (const Entity listener = listeners.front(); listener != k_no_entity) {
            const core::Mat4& matrix = listeners.get<const WorldTransform>(listener).matrix;
            m_device->set_listener(
                core::transform_point(matrix, core::Vec3{}),
                core::transform_direction(matrix, core::Vec3{0.0f, 0.0f, -1.0f}),
                core::transform_direction(matrix, core::Vec3{0.0f, 1.0f, 0.0f}));
        }

        for (auto it = m_source_voices.begin(); it != m_source_voices.end();) {
            if (!registry.valid(it->first) || !registry.all_of<AudioSource>(it->first)) {
                m_device->stop(it->second);
                it = m_source_voices.erase(it);
            } else {
                ++it;
            }
        }

        // Which sources have started is kept here, so the components stay plain settings.
        for (const auto [entity, source, transform] :
             registry.view<const AudioSource, const WorldTransform>().each()) {
            const core::Vec3 position = core::transform_point(transform.matrix, core::Vec3{});
            const auto playing = m_source_voices.find(entity);
            if (playing != m_source_voices.end()) {
                if (source.spatial) {
                    m_device->set_position(playing->second, position);
                }
                continue;
            }
            if (!source.autoplay || source.sound == nullptr) {
                continue;
            }
            audio::PlayDesc desc = source.desc;
            if (source.spatial) {
                desc.position = position;
            }
            const core::Result<audio::VoiceHandle> voice = m_device->play(source.sound->get(), desc);
            if (voice.has_value()) {
                m_source_voices.emplace(entity, voice.value());
            } else {
                DREAM_LOG_WARNING("Audio: entity {}'s sound: {}", entt::to_integral(entity),
                                  voice.error().message);
                m_source_voices.emplace(entity, audio::VoiceHandle{});
            }
        }
    }

    m_device->update();
    DREAM_PROFILE_VALUE("Audio voices", m_device->voice_count());
}

audio::VoiceHandle Audio::play(const AssetRef<SoundAsset>& sound, const audio::PlayDesc& desc) {
    if (m_device == nullptr || sound == nullptr) {
        return {};
    }
    const core::Result<audio::VoiceHandle> voice = m_device->play(sound->get(), desc);
    return voice.has_value() ? voice.value() : audio::VoiceHandle{};
}

audio::VoiceHandle Audio::play_at(const AssetRef<SoundAsset>& sound, core::Vec3 position,
                                  audio::PlayDesc desc) {
    desc.position = position;
    return play(sound, desc);
}

audio::VoiceHandle Audio::source_voice(Entity entity) const {
    const auto found = m_source_voices.find(entity);
    return found == m_source_voices.end() ? audio::VoiceHandle{} : found->second;
}

void Audio::stop(audio::VoiceHandle voice) {
    if (m_device != nullptr) {
        m_device->stop(voice);
    }
}

void Audio::set_bus_volume(audio::Bus bus, float volume) {
    if (m_device != nullptr) {
        m_device->set_bus_volume(bus, volume);
    }
}

float Audio::bus_volume(audio::Bus bus) const {
    return m_device != nullptr ? m_device->bus_volume(bus) : 0.0f;
}

} // namespace dream::runtime
