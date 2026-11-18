# Direct3D 12 and DXGI ship with Windows, and the Windows SDK that the compiler environment provides
# has their import libraries, so there is nothing for vcpkg to install.
if(NOT WIN32)
    message(FATAL_ERROR "The 'directx12' graphics backend can only be built for Windows. Remove it "
                        "from the [modules.${DREAM_PLATFORM}] table, or list it only under "
                        "[modules.windows].")
endif()

# Direct3D 12 runs shader model 5.0 DXBC too, so it loads the same files as the 'directx' backend,
# and a game listing both compiles its shaders once. d3dcompiler is only for reading a shader's
# registers and inputs.
dream_add_module(rhi directx12
    SOURCES src/directx12_device.cpp
    LIBRARIES d3d12 dxgi d3dcompiler dxguid
    SHADER_SUFFIX .dxbc
    SHADER_TARGET dxbc
    SHADER_PROFILE sm_5_0
)

if(DREAM_BUILD_TESTS)
    # Runs on WARP, Windows' software rasterizer, drawing to a hidden window, so a build server
    # without a GPU checks the whole backend. The test compiles its own HLSL with the system's fxc.
    dream_add_test(dream_rhi_directx12_tests
        SOURCES
            tests/test_main.cpp
            tests/directx12_device_test.cpp
        LIBRARIES
            dream::module_rhi_directx12
            d3dcompiler
            user32
    )
endif()
