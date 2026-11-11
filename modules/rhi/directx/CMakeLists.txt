# Direct3D 11 and DXGI ship with Windows, and the Windows SDK that the compiler environment provides
# has their import libraries, so there is nothing for vcpkg to install.
if(NOT WIN32)
    message(FATAL_ERROR "The 'directx' graphics backend can only be built for Windows. Remove it "
                        "from the [modules.${DREAM_PLATFORM}] table, or list it only under "
                        "[modules.windows].")
endif()

# Shaders are DXBC for shader model 5.0, which slangc produces through the fxc compiler that every
# Windows 10 machine has. The suffix matches Device::shader_file_suffix in directx_device.cpp.
dream_add_module(rhi directx
    SOURCES src/directx_device.cpp
    LIBRARIES d3d11 d3dcompiler dxguid
    SHADER_SUFFIX .dxbc
    SHADER_TARGET dxbc
    SHADER_PROFILE sm_5_0
)

if(DREAM_BUILD_TESTS)
    # Runs on WARP, Windows' software rasterizer, drawing to a hidden window, so a build server
    # without a GPU checks the whole backend. The test compiles its own HLSL with the system's fxc.
    dream_add_test(dream_rhi_directx_tests
        SOURCES
            tests/test_main.cpp
            tests/directx_device_test.cpp
        LIBRARIES
            dream::module_rhi_directx
            d3dcompiler
            user32
    )
endif()
