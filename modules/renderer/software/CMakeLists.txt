# Draws the scene on the CPU, with the software rhi module, for machines with no GPU (ADR 0039).
dream_add_module(renderer software SOURCES src/software_renderer.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_renderer_software_tests
        SOURCES
            tests/software_renderer_test.cpp
        LIBRARIES
            dream::module_renderer_software
            dream::module_rhi_software
    )
endif()
