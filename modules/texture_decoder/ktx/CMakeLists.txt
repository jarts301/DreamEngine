# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(Ktx CONFIG REQUIRED GLOBAL)

dream_add_module(texture_decoder ktx
    SOURCES src/ktx_texture_decoder.cpp
    LIBRARIES KTX::ktx
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_texture_decoder_ktx_tests
        SOURCES
            tests/test_main.cpp
            tests/ktx_texture_decoder_test.cpp
        LIBRARIES
            dream::module_texture_decoder_ktx
            KTX::ktx
    )
    # The runtime's fake graphics device.
    target_include_directories(dream_texture_decoder_ktx_tests PRIVATE
        "${DreamEngine_SOURCE_DIR}/runtime/tests")
endif()
