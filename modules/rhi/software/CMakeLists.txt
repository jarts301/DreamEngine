# A device that keeps resources in memory and draws on the CPU, for machines with no GPU. The
# software renderer draws with it, and the web platform shows its frames (ADR 0039).
dream_add_module(rhi software SOURCES src/software_device.cpp)
target_link_libraries(dream_module_rhi_software PRIVATE dream::interface_platform)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_rhi_software_tests
        SOURCES
            tests/software_device_test.cpp
        LIBRARIES
            dream::module_rhi_software
    )
endif()
