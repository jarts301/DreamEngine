# The null device loads SPIR-V, which slangc produces on every build machine, so automated runs
# still check that a game's compiled shaders load.
dream_add_module(rhi null
    SOURCES src/null_device.cpp
    SHADER_SUFFIX .spv
    SHADER_TARGET spirv
    SHADER_PROFILE spirv_1_3
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_rhi_null_tests
        SOURCES
            tests/test_main.cpp
            tests/null_device_test.cpp
        LIBRARIES
            dream::module_rhi_null
    )
endif()
