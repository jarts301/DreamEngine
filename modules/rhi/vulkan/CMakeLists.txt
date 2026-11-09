# GLOBAL because the game's final link needs these targets, and it happens in another directory.
find_package(VulkanHeaders CONFIG REQUIRED GLOBAL)
find_package(volk CONFIG REQUIRED GLOBAL)
find_package(VulkanMemoryAllocator CONFIG REQUIRED GLOBAL)

dream_add_module(rhi vulkan
    SOURCES src/vulkan_device.cpp
    LIBRARIES Vulkan::Headers volk::volk_headers GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS}
)

# Vulkan's structs have many optional fields, and leaving them zeroed is the normal way to use them.
if(NOT MSVC)
    target_compile_options(dream_module_rhi_vulkan PRIVATE -Wno-missing-field-initializers)
endif()

target_compile_definitions(dream_module_rhi_vulkan PRIVATE
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    $<$<PLATFORM_ID:Android>:VK_USE_PLATFORM_ANDROID_KHR>
)
