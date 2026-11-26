dream_add_module(asset_source bundles SOURCES src/bundles_asset_source.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_asset_source_bundles_tests
        SOURCES
            tests/test_main.cpp
            tests/bundles_asset_source_test.cpp
        LIBRARIES
            dream::module_asset_source_bundles
    )
    # The runtime's fake platform.
    target_include_directories(dream_asset_source_bundles_tests PRIVATE
        "${DreamEngine_SOURCE_DIR}/runtime/tests")
endif()
