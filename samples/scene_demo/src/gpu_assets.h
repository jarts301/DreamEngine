#pragma once

#include "dream/core/config.h"
#include "dream/rhi/rhi.h"
#include "dream/runtime/asset_manager.h"

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// Graphics objects as assets, so they are shared and freed automatically. The renderer will take
// over this job in a later phase.

template <typename Handle, void (dream::rhi::Device::*Destroy)(Handle)> class OwnedGpuResource {
public:
    OwnedGpuResource(dream::rhi::Device& device, Handle handle)
        : m_device(&device), m_handle(handle) {}

    OwnedGpuResource(OwnedGpuResource&& other) noexcept
        : m_device(other.m_device), m_handle(std::exchange(other.m_handle, Handle{})) {}

    OwnedGpuResource& operator=(OwnedGpuResource&&) = delete;

    ~OwnedGpuResource() {
        if (m_handle.is_valid()) {
            (m_device->*Destroy)(m_handle);
        }
    }

    [[nodiscard]] Handle get() const noexcept {
        return m_handle;
    }

private:
    dream::rhi::Device* m_device;
    Handle m_handle;
};

using OwnedShader = OwnedGpuResource<dream::rhi::ShaderHandle, &dream::rhi::Device::destroy_shader>;
using OwnedPipeline =
    OwnedGpuResource<dream::rhi::PipelineHandle, &dream::rhi::Device::destroy_pipeline>;

struct ShaderAsset {
    OwnedShader shader;
};

struct PipelineAsset {
    // Declared first, so the shaders outlive the pipeline made from them.
    dream::runtime::AssetRef<ShaderAsset> vertex_shader;
    dream::runtime::AssetRef<ShaderAsset> fragment_shader;
    OwnedPipeline pipeline;
};

// The stage comes from the name: "something.vert" or "something.frag".
inline dream::core::Result<ShaderAsset> load_shader(dream::rhi::Device& device,
                                                    dream::runtime::AssetLoadContext& context) {
    const auto code =
        context.files.read(std::format("{}{}", context.path, device.shader_file_suffix()));
    if (!code.has_value()) {
        return code.error();
    }

    const dream::rhi::ShaderStage stage = context.path.ends_with(".vert")
                                              ? dream::rhi::ShaderStage::Vertex
                                              : dream::rhi::ShaderStage::Fragment;
    const auto shader = device.create_shader(stage, code.value());
    if (!shader.has_value()) {
        return shader.error();
    }
    return ShaderAsset{OwnedShader(device, shader.value())};
}

inline dream::core::Result<PipelineAsset>
load_pipeline(dream::rhi::Device& device, dream::runtime::AssetLoadContext& context,
              std::uint32_t vertex_stride,
              std::span<const dream::rhi::VertexAttribute> attributes) {
    using namespace dream;

    const auto file = context.files.read(context.path);
    if (!file.has_value()) {
        return file.error();
    }
    const auto config = core::Config::parse(
        std::string_view(reinterpret_cast<const char*>(file.value().data()), file.value().size()));
    if (!config.has_value()) {
        return config.error();
    }

    const auto load_stage = [&](std::string_view key) -> core::Result<runtime::AssetRef<ShaderAsset>> {
        const auto path = config.value().get_string(key);
        if (!path.has_value()) {
            return core::Error{core::ErrorCode::ParseError, std::format("'{}' is missing", key)};
        }
        return context.assets.load<ShaderAsset>(*path);
    };

    auto vertex_shader = load_stage("vertex_shader");
    if (!vertex_shader.has_value()) {
        return vertex_shader.error();
    }
    auto fragment_shader = load_stage("fragment_shader");
    if (!fragment_shader.has_value()) {
        return fragment_shader.error();
    }

    const auto pipeline = device.create_pipeline(rhi::PipelineDesc{
        .vertex_shader = vertex_shader.value()->shader.get(),
        .fragment_shader = fragment_shader.value()->shader.get(),
        .vertex_stride = vertex_stride,
        .attributes = attributes,
    });
    if (!pipeline.has_value()) {
        return pipeline.error();
    }

    return PipelineAsset{std::move(vertex_shader).value(), std::move(fragment_shader).value(),
                         OwnedPipeline(device, pipeline.value())};
}
