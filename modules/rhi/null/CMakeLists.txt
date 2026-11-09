dream_add_module(rhi null SOURCES src/null_device.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_rhi_null_tests
        SOURCES
            tests/test_main.cpp
            tests/null_device_test.cpp
        LIBRARIES
            dream::module_rhi_null
    )
endif()
