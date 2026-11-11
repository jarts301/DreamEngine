# Build settings shared by every engine target. See docs/adr/0002-cpp-subset.md.

function(dream_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE
            _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN)
        if(DREAM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # Engine structs give every field a default, so designated initializers name only the
        # fields that differ, which Clang would otherwise warn about.
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-exceptions -fno-rtti
            -Wno-missing-designated-field-initializers)
        if(DREAM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Usage: dream_add_library(<name> SOURCES <files...> [LIBRARIES <third-party...>])
#
# An in-house library in libraries/<name>, which backends build on like any third-party library.
# It uses nothing from the engine, only the C++ standard library and its own LIBRARIES, but it is
# built with the engine's settings. Its public headers are in include/<name>/.
function(dream_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES")

    set(target dream_library_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(dream::library_${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC include)
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES})
    dream_configure_target(${target})
endfunction()

# Usage: dream_add_test(<name> SOURCES <files...> [LIBRARIES <targets...>])
#
# Tests are always compiled. They are registered with CTest only when the build machine can run
# them, so cross-compiled Android tests are built but not executed.
function(dream_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES")

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBRARIES} doctest::doctest)
    # Tests build without exceptions, like the engine. In this mode a failed REQUIRE ends the run.
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
    dream_configure_target(${name})

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME ${name} COMMAND ${name})
    endif()
endfunction()
