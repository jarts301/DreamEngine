#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_rhi_null(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::rhi;

namespace {

constexpr std::array<std::byte, 4> k_bytes{};
constexpr platform::SurfaceSize k_size{640, 480};

std::unique_ptr<Device> make_device() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_rhi_null(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<Device>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

PipelineHandle make_pipeline(Device& device) {
    const ShaderHandle shader = device.create_shader(ShaderStage::Vertex, k_bytes).value();
    PipelineDesc desc;
    desc.vertex_shader = shader;
    desc.fragment_shader = shader;
    return device.create_pipeline(desc).value();
}

} // namespace

TEST_CASE("Null device has nothing to draw to without a surface") {
    const auto device = make_device();

    const auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame.value() == nullptr);
}

TEST_CASE("Null device gives a frame only while a usable surface is attached") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());

    auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    REQUIRE(frame.value() != nullptr);
    REQUIRE(device->end_frame().has_value());

    device->resize_surface({0, 0});
    frame = device->begin_frame();
    CHECK(frame.value() == nullptr);

    device->resize_surface(k_size);
    device->detach_surface();
    frame = device->begin_frame();
    CHECK(frame.value() == nullptr);
}

TEST_CASE("Null device records a complete frame") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);
    const BufferHandle buffer = device->create_buffer(BufferUsage::Vertex, k_bytes).value();

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->bind_pipeline(pipeline);
    commands->bind_vertex_buffer(buffer);
    commands->bind_index_buffer(buffer, IndexFormat::UInt16);
    commands->push_constants(k_bytes);
    commands->draw_indexed(3);
    commands->end_render_pass();

    CHECK(device->end_frame().has_value());
}

TEST_CASE("Null device keeps pipelines across a reattached surface") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);

    device->detach_surface();
    REQUIRE(device->attach_surface({}, k_size).has_value());

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->bind_pipeline(pipeline);
    CHECK(device->end_frame().has_value());
}

TEST_CASE("Null device rejects invalid resources") {
    const auto device = make_device();

    CHECK_FALSE(device->create_buffer(BufferUsage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, {}).has_value());

    SUBCASE("a pipeline before any surface") {
        const ShaderHandle shader = device->create_shader(ShaderStage::Vertex, k_bytes).value();
        PipelineDesc desc;
        desc.vertex_shader = shader;
        desc.fragment_shader = shader;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
    SUBCASE("a pipeline with a destroyed shader") {
        REQUIRE(device->attach_surface({}, k_size).has_value());
        const ShaderHandle shader = device->create_shader(ShaderStage::Vertex, k_bytes).value();
        device->destroy_shader(shader);
        PipelineDesc desc;
        desc.vertex_shader = shader;
        desc.fragment_shader = shader;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
}

TEST_CASE("Null device ignores destroying a handle twice") {
    const auto device = make_device();
    const BufferHandle buffer = device->create_buffer(BufferUsage::Vertex, k_bytes).value();

    device->destroy_buffer(buffer);
    device->destroy_buffer(buffer);
    device->destroy_buffer(BufferHandle{});
}
