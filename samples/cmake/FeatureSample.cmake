# Builds a feature sample and its checks from one test configuration (samples/README.md).
#
# Usage: dream_add_feature_sample(<name> SOURCES <files...> [ASSETS <directories...>]
#                                 APPLICATION_ID <id> DISPLAY_NAME <text>
#                                 NULL_PASS <regex> [FRAMES <n>] [WARP])
#
# Makes dream_<name>, the game itself from dream.toml, and its checks from tests/check.toml.in:
#
# - dream_<name>_<backend>_check for vulkan, and on Windows directx, directx12, opengl and
#   opengles: frame FRAMES (60 by default) on the real GPU, compared with
#   tests/reference/screenshots/<name>.png. Not CTests, because build servers have no GPU.
# - dream_<name>_null, a CTest with no display or GPU, passing when its output matches NULL_PASS.
# - With WARP, dream_<name>_warp_check, a CTest comparing the picture on Windows' software
#   rasterizer, so build servers without a GPU compare it too (ADR 0021).
#
# tests/check.toml.in is a whole configuration in which @RHI@ is the graphics backend, @RHI_TABLE@
# its settings, @SCREENSHOT@ the [screenshot] table (empty for the null check) and @QUIT_AFTER@ the
# frames it runs. Every target can include the samples' shared headers, in samples/common.
set(DREAM_SAMPLES_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../common")

function(dream_add_feature_sample name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "WARP" "APPLICATION_ID;DISPLAY_NAME;NULL_PASS;FRAMES"
        "SOURCES;ASSETS")
    if(NOT ARG_FRAMES)
        set(ARG_FRAMES 60)
    endif()
    dream_add_game(dream_${name} SOURCES ${ARG_SOURCES} CONFIG dream.toml ASSETS ${ARG_ASSETS}
        APPLICATION_ID ${ARG_APPLICATION_ID} DISPLAY_NAME "${ARG_DISPLAY_NAME}")
    target_include_directories(dream_${name} PRIVATE "${DREAM_SAMPLES_COMMON_DIR}")
    if(NOT DREAM_BUILD_TESTS)
        return()
    endif()

    set(template "${CMAKE_CURRENT_SOURCE_DIR}/tests/check.toml.in")
    set(config_dir "${CMAKE_CURRENT_BINARY_DIR}/check_configs")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${template}")

    # backend, the rhi module, its settings, and whether the picture is checked.
    set(checks "vulkan|vulkan|[rhi.vulkan]\nvalidation = true")
    if(WIN32)
        list(APPEND checks
            "directx|directx|[rhi.directx]\ndebug = true"
            "directx12|directx12|[rhi.directx12]\ndebug = true"
            "opengl|opengl|[rhi.opengl]\ndebug = true"
            "opengles|opengles|[rhi.opengles]\ndebug = true")
        if(ARG_WARP AND NOT CMAKE_CROSSCOMPILING)
            list(APPEND checks "warp|directx|[rhi.directx]\nsoftware = true\ndebug = false")
        endif()
    endif()
    list(APPEND checks "null|null|")

    foreach(check IN LISTS checks)
        string(REPLACE "|" ";" parts "${check}")
        list(GET parts 0 backend)
        list(GET parts 1 RHI)
        list(LENGTH parts count)
        set(RHI_TABLE "")
        if(count GREATER 2)
            list(GET parts 2 RHI_TABLE)
            string(REPLACE "\\n" "\n" RHI_TABLE "${RHI_TABLE}")
        endif()
        if(backend STREQUAL "null")
            set(SCREENSHOT "")
            set(QUIT_AFTER 5)
            set(target dream_${name}_null)
        else()
            # The warp check ends at the compared frame, so CTest gets its result.
            if(backend STREQUAL "warp")
                set(quit true)
            else()
                set(quit false)
            endif()
            set(SCREENSHOT "# Compares a frame with the reference image, within a tolerance. See docs/adr/0021.
[screenshot]
frame = ${ARG_FRAMES}
save = \"${name}_${backend}_screenshot.png\"
reference = \"assets:/screenshots/${name}.png\"
tolerance = 24
max_differing = 0.01
quit = ${quit}")
            math(EXPR QUIT_AFTER "${ARG_FRAMES} + 60")
            set(target dream_${name}_${backend}_check)
        endif()
        configure_file("${template}" "${config_dir}/${backend}.toml" @ONLY)
        dream_add_game(${target} SOURCES ${ARG_SOURCES} CONFIG "${config_dir}/${backend}.toml"
            ASSETS ${ARG_ASSETS} tests/reference)
        target_include_directories(${target} PRIVATE "${DREAM_SAMPLES_COMMON_DIR}")
    endforeach()

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_${name}_null COMMAND dream_${name}_null)
        set_tests_properties(dream_${name}_null PROPERTIES
            PASS_REGULAR_EXPRESSION "${ARG_NULL_PASS}"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
        if(TARGET dream_${name}_warp_check)
            add_test(NAME dream_${name}_warp_check COMMAND dream_${name}_warp_check)
            set_tests_properties(dream_${name}_warp_check PROPERTIES
                PASS_REGULAR_EXPRESSION "Screenshot matches")
        endif()
    endif()
endfunction()
