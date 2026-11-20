# The forward renderer, built with DREAM_FORWARD_PLUS: many point and spot lights, sorted into
# clusters of the view (ADR 0037). The same source and shaders as 'forward', which a game listing
# both compiles once.
set(forward_dir "${CMAKE_CURRENT_SOURCE_DIR}/../forward")
dream_add_module(renderer forward_plus
    SOURCES ${forward_dir}/src/forward_renderer.cpp
    SHADERS
        ${forward_dir}/shaders/forward.slang
        ${forward_dir}/shaders/forward_masked.slang
        ${forward_dir}/shaders/forward_skinned.slang
        ${forward_dir}/shaders/forward_masked_skinned.slang
        ${forward_dir}/shaders/shadow.slang
        ${forward_dir}/shaders/shadow_masked.slang
        ${forward_dir}/shaders/shadow_skinned.slang
        ${forward_dir}/shaders/shadow_masked_skinned.slang
        ${forward_dir}/shaders/tonemap.slang
        ${forward_dir}/shaders/debug_lines.slang
        ${forward_dir}/shaders/ui.slang
)
target_compile_definitions(dream_module_renderer_forward_plus PRIVATE DREAM_FORWARD_PLUS)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_renderer_forward_plus_tests
        SOURCES
            ${forward_dir}/tests/test_main.cpp
            tests/forward_plus_test.cpp
        LIBRARIES
            dream::module_renderer_forward_plus
    )
    target_include_directories(dream_renderer_forward_plus_tests PRIVATE ${forward_dir}/tests)
endif()
