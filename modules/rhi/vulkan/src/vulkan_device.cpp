#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

// volk loads Vulkan at run time, so the program starts on machines without a Vulkan driver and can
// fall back to another backend.
#define VOLK_IMPLEMENTATION
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace rhi;

constexpr std::uint32_t k_frames_in_flight = 2;
constexpr const char* k_validation_layer = "VK_LAYER_KHRONOS_validation";

#if defined(NDEBUG)
constexpr bool k_validation_by_default = false;
#else
constexpr bool k_validation_by_default = true;
#endif

core::Error vulkan_error(std::string_view action, VkResult result) {
    return core::Error{core::ErrorCode::Unknown,
                       std::format("{} failed with Vulkan error {}", action,
                                   static_cast<int>(result))};
}

core::Error invalid(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        DREAM_LOG_ERROR("Vulkan: {}", data->pMessage);
    } else {
        DREAM_LOG_WARNING("Vulkan: {}", data->pMessage);
    }
    return VK_FALSE;
}

struct BufferResource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct ShaderResource {
    VkShaderModule module = VK_NULL_HANDLE;
};

struct PipelineResource {
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct FrameResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
};

VkFormat to_vulkan(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

class VulkanDevice final : public Device, private CommandList {
public:
    VulkanDevice() = default;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    ~VulkanDevice() override {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            detach_surface();
            for (FrameResources& frame : m_frames) {
                vkDestroyCommandPool(m_device, frame.pool, nullptr);
                vkDestroyFence(m_device, frame.in_flight, nullptr);
                vkDestroySemaphore(m_device, frame.image_available, nullptr);
            }
            vkDestroyRenderPass(m_device, m_render_pass, nullptr);
            vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
            vmaDestroyAllocator(m_allocator);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE) {
            if (m_messenger != VK_NULL_HANDLE) {
                vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
            }
            vkDestroyInstance(m_instance, nullptr);
        }
    }

    [[nodiscard]] core::Result<void> initialize(const core::Config& config) {
        if (const VkResult result = volkInitialize(); result != VK_SUCCESS) {
            return vulkan_error("Loading the Vulkan driver", result);
        }
        if (auto result = create_instance(config); !result.has_value()) {
            return result;
        }
        if (auto result = choose_physical_device(); !result.has_value()) {
            return result;
        }
        if (auto result = create_device(); !result.has_value()) {
            return result;
        }
        return create_frame_resources();
    }

    core::Result<void> attach_surface(const platform::NativeWindow& window,
                                      platform::SurfaceSize size) override {
        detach_surface();

        if (auto result = create_surface(window); !result.has_value()) {
            return result;
        }

        VkBool32 can_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, m_queue_family, m_surface,
                                             &can_present);
        if (can_present != VK_TRUE) {
            detach_surface();
            return core::Error{core::ErrorCode::Unknown, "This GPU can't draw to this window"};
        }

        m_requested_size = size;
        m_surface_lost = false;
        m_needs_new_swapchain = false;
        if (auto result = create_swapchain(); !result.has_value()) {
            detach_surface();
            return result;
        }
        return {};
    }

    void detach_surface() override {
        if (m_surface == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(m_device);
        destroy_swapchain_targets();
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    void resize_surface(platform::SurfaceSize size) override {
        if (size != m_requested_size) {
            m_requested_size = size;
            m_needs_new_swapchain = true;
        }
    }

    std::string_view shader_file_suffix() const override {
        return ".spv";
    }

    core::Result<BufferHandle> create_buffer(BufferUsage usage,
                                             std::span<const std::byte> contents) override {
        if (contents.empty()) {
            return invalid("A buffer can't be empty");
        }

        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = contents.size(),
            .usage = static_cast<VkBufferUsageFlags>(usage == BufferUsage::Vertex
                                                         ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                         : VK_BUFFER_USAGE_INDEX_BUFFER_BIT),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        // Memory the CPU can write directly. Simple, and enough until the renderer adds staging
        // uploads for large meshes.
        const VmaAllocationCreateInfo allocation_info{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        BufferResource resource;
        VkResult result = vmaCreateBuffer(m_allocator, &buffer_info, &allocation_info,
                                          &resource.buffer, &resource.allocation, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error("Creating a buffer", result);
        }

        result = vmaCopyMemoryToAllocation(m_allocator, contents.data(), resource.allocation, 0,
                                           contents.size());
        if (result != VK_SUCCESS) {
            vmaDestroyBuffer(m_allocator, resource.buffer, resource.allocation);
            return vulkan_error("Filling a buffer", result);
        }
        return handle_cast<BufferTag>(m_buffers.create(resource));
    }

    core::Result<ShaderHandle> create_shader(ShaderStage /*stage*/,
                                             std::span<const std::byte> code) override {
        if (code.empty() || code.size() % sizeof(std::uint32_t) != 0) {
            return invalid("Shader code is not valid SPIR-V");
        }

        // The bytes may come from any file buffer, so they are copied to get the required alignment.
        std::vector<std::uint32_t> words(code.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), code.data(), code.size());

        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.size(),
            .pCode = words.data(),
        };
        ShaderResource resource;
        if (const VkResult result = vkCreateShaderModule(m_device, &info, nullptr, &resource.module);
            result != VK_SUCCESS) {
            return vulkan_error("Creating a shader", result);
        }
        return handle_cast<ShaderTag>(m_shaders.create(resource));
    }

    core::Result<PipelineHandle> create_pipeline(const PipelineDesc& desc) override {
        if (m_render_pass == VK_NULL_HANDLE) {
            return invalid("A pipeline needs a surface to have been attached");
        }
        const ShaderResource* const vertex =
            m_shaders.get(handle_cast<ShaderResource>(desc.vertex_shader));
        const ShaderResource* const fragment =
            m_shaders.get(handle_cast<ShaderResource>(desc.fragment_shader));
        if (vertex == nullptr || fragment == nullptr) {
            return invalid("A pipeline needs valid shaders");
        }

        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex->module,
                .pName = "main",
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment->module,
                .pName = "main",
            },
        };

        const VkVertexInputBindingDescription binding{
            .binding = 0,
            .stride = desc.vertex_stride,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        std::vector<VkVertexInputAttributeDescription> attributes;
        attributes.reserve(desc.attributes.size());
        for (const VertexAttribute& attribute : desc.attributes) {
            attributes.push_back(VkVertexInputAttributeDescription{
                .location = attribute.location,
                .binding = 0,
                .format = to_vulkan(attribute.format),
                .offset = attribute.offset,
            });
        }

        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &binding,
            .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        const VkPipelineRasterizationStateCreateInfo rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = static_cast<VkCullModeFlags>(desc.cull_back_faces ? VK_CULL_MODE_BACK_BIT
                                                                          : VK_CULL_MODE_NONE),
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkPipelineDepthStencilStateCreateInfo depth{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
        };
        const VkPipelineColorBlendAttachmentState blend_attachment{
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blend_attachment,
        };
        constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };

        const VkGraphicsPipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = m_pipeline_layout,
            .renderPass = m_render_pass,
        };

        PipelineResource resource;
        if (const VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info,
                                                              nullptr, &resource.pipeline);
            result != VK_SUCCESS) {
            return vulkan_error("Creating a pipeline", result);
        }
        return handle_cast<PipelineTag>(m_pipelines.create(resource));
    }

    // Destroying waits for the GPU to finish, which is simple and fine while destruction is rare.
    // A per-frame deletion queue can replace this if streaming ever needs it.
    void destroy_buffer(BufferHandle handle) override {
        const auto key = handle_cast<BufferResource>(handle);
        if (const BufferResource* const resource = m_buffers.get(key)) {
            vkDeviceWaitIdle(m_device);
            vmaDestroyBuffer(m_allocator, resource->buffer, resource->allocation);
            m_buffers.destroy(key);
        }
    }

    void destroy_shader(ShaderHandle handle) override {
        const auto key = handle_cast<ShaderResource>(handle);
        if (const ShaderResource* const resource = m_shaders.get(key)) {
            vkDestroyShaderModule(m_device, resource->module, nullptr);
            m_shaders.destroy(key);
        }
    }

    void destroy_pipeline(PipelineHandle handle) override {
        const auto key = handle_cast<PipelineResource>(handle);
        if (const PipelineResource* const resource = m_pipelines.get(key)) {
            vkDeviceWaitIdle(m_device);
            vkDestroyPipeline(m_device, resource->pipeline, nullptr);
            m_pipelines.destroy(key);
        }
    }

    core::Result<CommandList*> begin_frame() override {
        const auto no_frame = static_cast<CommandList*>(nullptr);

        // A lost surface needs the platform to provide a new window, through attach_surface.
        if (m_surface == VK_NULL_HANDLE || m_surface_lost) {
            return no_frame;
        }
        if (m_needs_new_swapchain || m_swapchain == VK_NULL_HANDLE) {
            if (auto result = recreate_swapchain(); !result.has_value()) {
                return result.error();
            }
            if (m_swapchain == VK_NULL_HANDLE) {
                return no_frame;
            }
        }

        FrameResources& frame = m_frames[m_frame_index];
        VkResult result =
            vkWaitForFences(m_device, 1, &frame.in_flight, VK_TRUE,
                            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            return vulkan_error("Waiting for the GPU", result);
        }

        result = vkAcquireNextImageKHR(m_device, m_swapchain,
                                       std::numeric_limits<std::uint64_t>::max(),
                                       frame.image_available, VK_NULL_HANDLE, &m_image_index);
        switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            // Suboptimal is normal when Android rotates our image for us, so it is not acted on.
            break;
        case VK_ERROR_OUT_OF_DATE_KHR:
            m_needs_new_swapchain = true;
            return no_frame;
        case VK_ERROR_SURFACE_LOST_KHR:
            m_surface_lost = true;
            return no_frame;
        default:
            return vulkan_error("Getting a window image", result);
        }

        // Only after a successful acquire, or the next wait above would never finish.
        vkResetFences(m_device, 1, &frame.in_flight);
        vkResetCommandPool(m_device, frame.pool, 0);

        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        result = vkBeginCommandBuffer(frame.commands, &begin_info);
        if (result != VK_SUCCESS) {
            return vulkan_error("Starting a frame", result);
        }
        m_recording = frame.commands;
        return static_cast<CommandList*>(this);
    }

    core::Result<void> end_frame() override {
        DREAM_VERIFY(m_recording != VK_NULL_HANDLE, "end_frame called without begin_frame");

        FrameResources& frame = m_frames[m_frame_index];
        VkResult result = vkEndCommandBuffer(frame.commands);
        m_recording = VK_NULL_HANDLE;
        if (result != VK_SUCCESS) {
            return vulkan_error("Finishing a frame", result);
        }

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSemaphore render_finished = m_render_finished[m_image_index];
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame.image_available,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &frame.commands,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &render_finished,
        };
        result = vkQueueSubmit(m_queue, 1, &submit, frame.in_flight);
        if (result != VK_SUCCESS) {
            return vulkan_error("Submitting a frame", result);
        }

        const VkPresentInfoKHR present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &render_finished,
            .swapchainCount = 1,
            .pSwapchains = &m_swapchain,
            .pImageIndices = &m_image_index,
        };
        result = vkQueuePresentKHR(m_queue, &present);
        m_frame_index = (m_frame_index + 1) % k_frames_in_flight;

        switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            return {};
        case VK_ERROR_OUT_OF_DATE_KHR:
            m_needs_new_swapchain = true;
            return {};
        case VK_ERROR_SURFACE_LOST_KHR:
            m_surface_lost = true;
            return {};
        default:
            return vulkan_error("Presenting a frame", result);
        }
    }

private:
    core::Result<void> create_instance(const core::Config& config) {
        std::uint32_t api_version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion != nullptr) {
            vkEnumerateInstanceVersion(&api_version);
        }
        if (api_version < VK_API_VERSION_1_1) {
            return core::Error{core::ErrorCode::Unknown, "Vulkan 1.1 is not available"};
        }

        std::vector<const char*> extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
        };

        std::vector<const char*> layers;
        const bool wants_validation =
            config.get_bool("rhi.vulkan.validation").value_or(k_validation_by_default);
        if (wants_validation && is_validation_available()) {
            layers.push_back(k_validation_layer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        const bool validating = !layers.empty();
        DREAM_LOG_INFO("Vulkan validation is {}", validating ? "on" : "off");

        const VkApplicationInfo application{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pEngineName = "DreamEngine",
            .apiVersion = VK_API_VERSION_1_1,
        };
        const VkInstanceCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application,
            .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        if (const VkResult result = vkCreateInstance(&info, nullptr, &m_instance);
            result != VK_SUCCESS) {
            return vulkan_error("Creating a Vulkan instance", result);
        }
        volkLoadInstance(m_instance);

        if (validating) {
            const VkDebugUtilsMessengerCreateInfoEXT messenger{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                .pfnUserCallback = &on_validation_message,
            };
            vkCreateDebugUtilsMessengerEXT(m_instance, &messenger, nullptr, &m_messenger);
        }
        return {};
    }

    [[nodiscard]] static bool is_validation_available() {
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        return std::ranges::any_of(layers, [](const VkLayerProperties& layer) {
            return std::string_view(layer.layerName) == k_validation_layer;
        });
    }

    core::Result<void> choose_physical_device() {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

        int best_score = 0;
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);

            const std::optional<std::uint32_t> family = find_graphics_queue(device);
            if (properties.apiVersion < VK_API_VERSION_1_1 || !family.has_value() ||
                !supports_swapchain(device)) {
                continue;
            }

            const int score =
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                m_physical_device = device;
                m_queue_family = *family;
            }
        }

        if (m_physical_device == VK_NULL_HANDLE) {
            return core::Error{core::ErrorCode::Unknown, "No suitable Vulkan GPU was found"};
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_physical_device, &properties);
        DREAM_LOG_INFO("Vulkan device: {} (Vulkan {}.{}.{})", properties.deviceName,
                       VK_API_VERSION_MAJOR(properties.apiVersion),
                       VK_API_VERSION_MINOR(properties.apiVersion),
                       VK_API_VERSION_PATCH(properties.apiVersion));
        return {};
    }

    [[nodiscard]] static std::optional<std::uint32_t> find_graphics_queue(VkPhysicalDevice device) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (std::uint32_t index = 0; index < count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool supports_swapchain(VkPhysicalDevice device) {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
        return std::ranges::any_of(extensions, [](const VkExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        });
    }

    core::Result<void> create_device() {
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = m_queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        const char* const extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        const VkDeviceCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = &extension,
        };
        if (const VkResult result = vkCreateDevice(m_physical_device, &info, nullptr, &m_device);
            result != VK_SUCCESS) {
            return vulkan_error("Creating a Vulkan device", result);
        }
        volkLoadDevice(m_device);
        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

        const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        };
        const VmaAllocatorCreateInfo allocator{
            .physicalDevice = m_physical_device,
            .device = m_device,
            .pVulkanFunctions = &functions,
            .instance = m_instance,
            .vulkanApiVersion = VK_API_VERSION_1_1,
        };
        if (const VkResult result = vmaCreateAllocator(&allocator, &m_allocator);
            result != VK_SUCCESS) {
            return vulkan_error("Creating the memory allocator", result);
        }

        // One layout for every pipeline, because the only shader input so far is push constants.
        const VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = k_max_push_constant_size,
        };
        const VkPipelineLayoutCreateInfo layout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        };
        if (const VkResult result =
                vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipeline_layout);
            result != VK_SUCCESS) {
            return vulkan_error("Creating the pipeline layout", result);
        }
        return {};
    }

    core::Result<void> create_frame_resources() {
        for (FrameResources& frame : m_frames) {
            const VkCommandPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                .queueFamilyIndex = m_queue_family,
            };
            VkResult result = vkCreateCommandPool(m_device, &pool_info, nullptr, &frame.pool);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a command pool", result);
            }

            const VkCommandBufferAllocateInfo commands_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = frame.pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            result = vkAllocateCommandBuffers(m_device, &commands_info, &frame.commands);
            if (result != VK_SUCCESS) {
                return vulkan_error("Allocating a command buffer", result);
            }

            const VkFenceCreateInfo fence_info{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            result = vkCreateFence(m_device, &fence_info, nullptr, &frame.in_flight);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a fence", result);
            }

            const VkSemaphoreCreateInfo semaphore_info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            };
            result = vkCreateSemaphore(m_device, &semaphore_info, nullptr, &frame.image_available);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a semaphore", result);
            }
        }
        return {};
    }

    core::Result<void> create_surface(const platform::NativeWindow& window) {
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (const auto* const win32 = std::get_if<platform::Win32Window>(&window)) {
            const VkWin32SurfaceCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                .hinstance = static_cast<HINSTANCE>(win32->instance),
                .hwnd = static_cast<HWND>(win32->window),
            };
            result = vkCreateWin32SurfaceKHR(m_instance, &info, nullptr, &m_surface);
        } else {
            return invalid("Vulkan needs a Windows window here");
        }
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (const auto* const android = std::get_if<platform::AndroidWindow>(&window)) {
            const VkAndroidSurfaceCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                .window = static_cast<ANativeWindow*>(android->window),
            };
            result = vkCreateAndroidSurfaceKHR(m_instance, &info, nullptr, &m_surface);
        } else {
            return invalid("Vulkan needs an Android window here");
        }
#else
        (void)window;
#endif
        if (result != VK_SUCCESS) {
            return vulkan_error("Creating a window surface", result);
        }
        return {};
    }

    core::Result<void> choose_formats() {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &count, formats.data());
        if (formats.empty()) {
            return core::Error{core::ErrorCode::Unknown, "This window has no usable formats"};
        }

        const auto find_format = [&](VkFormat wanted) {
            return std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& format) {
                return format.format == wanted &&
                       format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
        };

        // Pipelines are tied to the format, so after the first surface it has to stay the same.
        if (m_render_pass != VK_NULL_HANDLE) {
            if (find_format(m_surface_format.format) == formats.end()) {
                return core::Error{core::ErrorCode::Unknown,
                                   "The new window can't use the existing image format"};
            }
            return {};
        }

        m_surface_format = formats.front();
        for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
            if (const auto format = find_format(wanted); format != formats.end()) {
                m_surface_format = *format;
                break;
            }
        }

        m_depth_format = VK_FORMAT_UNDEFINED;
        for (const VkFormat candidate :
             {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(m_physical_device, candidate, &properties);
            if ((properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                m_depth_format = candidate;
                break;
            }
        }
        if (m_depth_format == VK_FORMAT_UNDEFINED) {
            return core::Error{core::ErrorCode::Unknown, "No usable depth format was found"};
        }

        return create_render_pass();
    }

    core::Result<void> create_render_pass() {
        const std::array attachments{
            VkAttachmentDescription{
                .format = m_surface_format.format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            },
            VkAttachmentDescription{
                .format = m_depth_format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            },
        };
        const VkAttachmentReference color_reference{
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        const VkAttachmentReference depth_reference{
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        const VkSubpassDescription subpass{
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_reference,
            .pDepthStencilAttachment = &depth_reference,
        };
        const VkSubpassDependency dependency{
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        };
        const VkRenderPassCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
            .pAttachments = attachments.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
        };
        if (const VkResult result = vkCreateRenderPass(m_device, &info, nullptr, &m_render_pass);
            result != VK_SUCCESS) {
            return vulkan_error("Creating the render pass", result);
        }
        return {};
    }

    core::Result<void> create_swapchain() {
        if (auto result = choose_formats(); !result.has_value()) {
            return result;
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface,
                                                                    &capabilities);
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            m_surface_lost = true;
            return {};
        }
        if (result != VK_SUCCESS) {
            return vulkan_error("Reading the window's capabilities", result);
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(m_requested_size.width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(m_requested_size.height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        // A minimized window has no size. The swapchain is created when it gets one again.
        if (extent.width == 0 || extent.height == 0) {
            m_needs_new_swapchain = true;
            return {};
        }

        std::uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0) {
            image_count = std::min(image_count, capabilities.maxImageCount);
        }

        // Asking for no rotation lets the system compositor rotate for us on Android. Rendering in
        // the display's orientation would be faster, and can be added without changing the API.
        const VkSurfaceTransformFlagBitsKHR transform =
            (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
                ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                : capabilities.currentTransform;

        VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        for (const VkCompositeAlphaFlagBitsKHR candidate :
             {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
              VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
                composite_alpha = candidate;
                break;
            }
        }

        const VkSwapchainKHR previous = m_swapchain;
        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = m_surface,
            .minImageCount = image_count,
            .imageFormat = m_surface_format.format,
            .imageColorSpace = m_surface_format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = transform,
            .compositeAlpha = composite_alpha,
            // Always available, and paces the game to the display.
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
            .oldSwapchain = previous,
        };
        result = vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain);
        if (previous != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, previous, nullptr);
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            m_swapchain = VK_NULL_HANDLE;
            m_surface_lost = true;
            return {};
        }
        if (result != VK_SUCCESS) {
            m_swapchain = VK_NULL_HANDLE;
            return vulkan_error("Creating the swapchain", result);
        }

        m_extent = extent;
        m_needs_new_swapchain = false;
        return create_swapchain_targets();
    }

    core::Result<void> create_swapchain_targets() {
        std::uint32_t count = 0;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
        m_images.resize(count);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_images.data());

        const VkImageCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = m_depth_format,
            .extent = {m_extent.width, m_extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        };
        const VmaAllocationCreateInfo depth_allocation{.usage = VMA_MEMORY_USAGE_AUTO};
        VkResult result = vmaCreateImage(m_allocator, &depth_info, &depth_allocation, &m_depth_image,
                                         &m_depth_allocation, nullptr);
        if (result != VK_SUCCESS) {
            return vulkan_error("Creating the depth image", result);
        }

        const auto make_view = [&](VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                   VkImageView& view) {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .subresourceRange = {aspect, 0, 1, 0, 1},
            };
            return vkCreateImageView(m_device, &info, nullptr, &view);
        };

        result = make_view(m_depth_image, m_depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, m_depth_view);
        if (result != VK_SUCCESS) {
            return vulkan_error("Creating the depth view", result);
        }

        m_views.assign(count, VK_NULL_HANDLE);
        m_framebuffers.assign(count, VK_NULL_HANDLE);
        m_render_finished.assign(count, VK_NULL_HANDLE);
        for (std::uint32_t index = 0; index < count; ++index) {
            result = make_view(m_images[index], m_surface_format.format, VK_IMAGE_ASPECT_COLOR_BIT,
                               m_views[index]);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a window image view", result);
            }

            const std::array attachments{m_views[index], m_depth_view};
            const VkFramebufferCreateInfo framebuffer{
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = m_render_pass,
                .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
                .pAttachments = attachments.data(),
                .width = m_extent.width,
                .height = m_extent.height,
                .layers = 1,
            };
            result = vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_framebuffers[index]);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a framebuffer", result);
            }

            const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            result = vkCreateSemaphore(m_device, &semaphore, nullptr, &m_render_finished[index]);
            if (result != VK_SUCCESS) {
                return vulkan_error("Creating a semaphore", result);
            }
        }
        return {};
    }

    void destroy_swapchain_targets() {
        for (const VkFramebuffer framebuffer : m_framebuffers) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
        for (const VkImageView view : m_views) {
            vkDestroyImageView(m_device, view, nullptr);
        }
        for (const VkSemaphore semaphore : m_render_finished) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
        m_framebuffers.clear();
        m_views.clear();
        m_render_finished.clear();
        m_images.clear();

        vkDestroyImageView(m_device, m_depth_view, nullptr);
        m_depth_view = VK_NULL_HANDLE;
        if (m_depth_image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_allocator, m_depth_image, m_depth_allocation);
            m_depth_image = VK_NULL_HANDLE;
            m_depth_allocation = VK_NULL_HANDLE;
        }
    }

    core::Result<void> recreate_swapchain() {
        vkDeviceWaitIdle(m_device);
        destroy_swapchain_targets();
        return create_swapchain();
    }

    // CommandList

    void begin_render_pass(const ClearColor& color) override {
        const std::array clear_values{
            VkClearValue{.color = {.float32 = {color.r, color.g, color.b, color.a}}},
            VkClearValue{.depthStencil = {1.0f, 0}},
        };
        const VkRenderPassBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = m_render_pass,
            .framebuffer = m_framebuffers[m_image_index],
            .renderArea = {{0, 0}, m_extent},
            .clearValueCount = static_cast<std::uint32_t>(clear_values.size()),
            .pClearValues = clear_values.data(),
        };
        vkCmdBeginRenderPass(m_recording, &info, VK_SUBPASS_CONTENTS_INLINE);

        // A negative height flips Vulkan's +Y-down clip space to the engine's +Y up (ADR 0005).
        const VkViewport viewport{
            .x = 0.0f,
            .y = static_cast<float>(m_extent.height),
            .width = static_cast<float>(m_extent.width),
            .height = -static_cast<float>(m_extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        const VkRect2D scissor{{0, 0}, m_extent};
        vkCmdSetViewport(m_recording, 0, 1, &viewport);
        vkCmdSetScissor(m_recording, 0, 1, &scissor);
    }

    void end_render_pass() override {
        vkCmdEndRenderPass(m_recording);
    }

    void bind_pipeline(PipelineHandle handle) override {
        const PipelineResource* const resource =
            m_pipelines.get(handle_cast<PipelineResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a pipeline that doesn't exist");
        vkCmdBindPipeline(m_recording, VK_PIPELINE_BIND_POINT_GRAPHICS, resource->pipeline);
    }

    void bind_vertex_buffer(BufferHandle handle) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_recording, 0, 1, &resource->buffer, &offset);
    }

    void bind_index_buffer(BufferHandle handle, IndexFormat format) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        vkCmdBindIndexBuffer(m_recording, resource->buffer, 0,
                             format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16
                                                           : VK_INDEX_TYPE_UINT32);
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
        vkCmdPushConstants(m_recording, m_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           static_cast<std::uint32_t>(data.size()), data.data());
    }

    void draw_indexed(std::uint32_t index_count) override {
        vkCmdDrawIndexed(m_recording, index_count, 1, 0, 0, 0);
    }

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queue_family = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass m_render_pass = VK_NULL_HANDLE;

    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR m_surface_format{};
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    platform::SurfaceSize m_requested_size;
    bool m_needs_new_swapchain = false;
    bool m_surface_lost = false;

    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkFramebuffer> m_framebuffers;
    std::vector<VkSemaphore> m_render_finished;
    VkImage m_depth_image = VK_NULL_HANDLE;
    VmaAllocation m_depth_allocation = VK_NULL_HANDLE;
    VkImageView m_depth_view = VK_NULL_HANDLE;

    std::array<FrameResources, k_frames_in_flight> m_frames{};
    std::uint32_t m_frame_index = 0;
    std::uint32_t m_image_index = 0;
    VkCommandBuffer m_recording = VK_NULL_HANDLE;

    core::HandlePool<BufferResource> m_buffers;
    core::HandlePool<ShaderResource> m_shaders;
    core::HandlePool<PipelineResource> m_pipelines;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& config) {
    auto device = std::make_unique<VulkanDevice>();
    if (core::Result<void> result = device->initialize(config); !result.has_value()) {
        return result.error();
    }
    return std::unique_ptr<Device>{std::move(device)};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_vulkan(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "vulkan", .create = &create});
}

} // namespace dream::modules
