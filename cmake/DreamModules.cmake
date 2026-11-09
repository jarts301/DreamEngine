# Declares interfaces and backends, and links the ones a project selects.
# See docs/adr/0006-module-system.md.

include(${CMAKE_CURRENT_LIST_DIR}/DreamModuleConfig.cmake)

if(ANDROID)
    set(dream_default_platform android)
elseif(IOS)
    set(dream_default_platform ios)
elseif(WIN32)
    set(dream_default_platform windows)
elseif(APPLE)
    set(dream_default_platform macos)
elseif(UNIX)
    set(dream_default_platform linux)
else()
    set(dream_default_platform "")
endif()
set(DREAM_PLATFORM "${dream_default_platform}" CACHE STRING
    "The [modules.<platform>] table used from project configurations")

# Usage: dream_add_interface(<name>)
function(dream_add_interface name)
    add_library(dream_interface_${name} INTERFACE)
    add_library(dream::interface_${name} ALIAS dream_interface_${name})
    target_include_directories(dream_interface_${name} INTERFACE include)
    target_link_libraries(dream_interface_${name} INTERFACE dream::core)
    target_compile_features(dream_interface_${name} INTERFACE cxx_std_20)
endfunction()

# Usage: dream_add_module(<interface> <backend> SOURCES <files...> [LIBRARIES <third-party...>])
#
# The sources must define dream::modules::register_<interface>_<backend>, which the generated
# registration code calls.
function(dream_add_module interface backend)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "SOURCES;LIBRARIES")

    set(target dream_module_${interface}_${backend})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(dream::module_${interface}_${backend} ALIAS ${target})
    target_link_libraries(${target}
        PUBLIC dream::core dream::interface_${interface}
        PRIVATE ${ARG_LIBRARIES})
    dream_configure_target(${target})
endfunction()

# Usage: dream_link_modules(<target> CONFIG <toml-file>)
#
# Builds the backends listed for the current platform, links them into <target>, and generates the
# code that registers them and embeds the configuration. Backends that aren't listed are not
# compiled at all.
function(dream_link_modules target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CONFIG" "")
    if(NOT ARG_CONFIG)
        message(FATAL_ERROR "dream_link_modules(${target}) needs a CONFIG file.")
    endif()
    cmake_path(ABSOLUTE_PATH ARG_CONFIG NORMALIZE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_CONFIG}")

    dream_read_module_config("${ARG_CONFIG}" "${DREAM_PLATFORM}" config)
    if(NOT config_FOUND)
        message(FATAL_ERROR "${ARG_CONFIG} has no [modules.${DREAM_PLATFORM}] table.")
    endif()

    # MSVC rejects string literals over about 16 KB, and the file is embedded as one.
    file(SIZE "${ARG_CONFIG}" config_size)
    if(config_size GREATER 16000)
        message(FATAL_ERROR "${ARG_CONFIG} is larger than the 16000 byte limit.")
    endif()

    set(DREAM_MODULE_DECLARATIONS "")
    set(DREAM_MODULE_CALLS "")
    set(module_targets "")

    foreach(interface IN LISTS config_INTERFACES)
        foreach(backend IN LISTS config_BACKENDS_${interface})
            if(NOT TARGET dream_module_${interface}_${backend})
                set(module_dir "${DreamEngine_SOURCE_DIR}/modules/${interface}/${backend}")
                if(NOT EXISTS "${module_dir}/CMakeLists.txt")
                    file(GLOB available RELATIVE "${DreamEngine_SOURCE_DIR}/modules/${interface}"
                        "${DreamEngine_SOURCE_DIR}/modules/${interface}/*/CMakeLists.txt")
                    list(TRANSFORM available REPLACE "/CMakeLists.txt$" "")
                    message(FATAL_ERROR "${ARG_CONFIG} selects unknown backend '${backend}' for "
                                        "interface '${interface}'. Available: ${available}")
                endif()
                add_subdirectory("${module_dir}"
                    "${DreamEngine_BINARY_DIR}/modules/${interface}/${backend}")
            endif()

            set(function_name "register_${interface}_${backend}")
            string(APPEND DREAM_MODULE_DECLARATIONS
                "[[nodiscard]] core::Result<void> ${function_name}(core::ModuleRegistry& registry);\n")
            string(APPEND DREAM_MODULE_CALLS
                "    if (Result<void> result = modules::${function_name}(registry); !result) {\n"
                "        return result;\n"
                "    }\n")
            list(APPEND module_targets dream::module_${interface}_${backend})
        endforeach()
    endforeach()

    file(READ "${ARG_CONFIG}" DREAM_MODULE_CONFIG_TEXT)
    set(DREAM_MODULE_CONFIG_PATH "${ARG_CONFIG}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_module_registration.cpp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/module_registration.cpp.in" "${generated}"
        @ONLY)
    target_sources(${target} PRIVATE "${generated}")
    target_link_libraries(${target} PRIVATE dream::core ${module_targets})
endfunction()
