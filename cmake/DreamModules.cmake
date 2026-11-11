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

# Usage: dream_add_module(<interface> <backend> SOURCES <files...> [LIBRARIES <third-party...>]
#                         [SHADER_SUFFIX <suffix> SHADER_TARGET <target> SHADER_PROFILE <profile>])
#
# The sources must define dream::modules::register_<interface>_<backend>, which the generated
# registration code calls.
#
# A graphics backend loads one compiled shader format, and declares it with the three SHADER_
# options: the suffix its Device::shader_file_suffix returns, and the slangc target and profile that
# produce that format. dream_add_game compiles a game's shaders once for every format that the
# game's selected backends load, so a project switches graphics backends by editing only its module
# lists. See docs/adr/0013-directx-and-shader-formats.md.
function(dream_add_module interface backend)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "SHADER_SUFFIX;SHADER_TARGET;SHADER_PROFILE"
        "SOURCES;LIBRARIES")

    set(target dream_module_${interface}_${backend})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(dream::module_${interface}_${backend} ALIAS ${target})
    target_link_libraries(${target}
        PUBLIC dream::core dream::interface_${interface}
        PRIVATE ${ARG_LIBRARIES})
    dream_configure_target(${target})

    if(ARG_SHADER_SUFFIX OR ARG_SHADER_TARGET OR ARG_SHADER_PROFILE)
        if(NOT (ARG_SHADER_SUFFIX AND ARG_SHADER_TARGET AND ARG_SHADER_PROFILE))
            message(FATAL_ERROR "dream_add_module(${interface} ${backend}) needs SHADER_SUFFIX, "
                                "SHADER_TARGET and SHADER_PROFILE together.")
        endif()
        set_target_properties(${target} PROPERTIES
            DREAM_SHADER_SUFFIX "${ARG_SHADER_SUFFIX}"
            DREAM_SHADER_TARGET "${ARG_SHADER_TARGET}"
            DREAM_SHADER_PROFILE "${ARG_SHADER_PROFILE}")
    endif()
endfunction()

# Usage: dream_module_shader_formats(<target> <out-var>)
#
# Lists the shader formats that the modules linked into <target> by dream_link_modules load. Each
# entry is "<suffix>|<slangc target>|<slangc profile>", and each format appears once even when
# several backends share it.
function(dream_module_shader_formats target out_var)
    get_target_property(module_targets ${target} DREAM_MODULE_TARGETS)
    set(formats "")
    if(module_targets)
        foreach(module_target IN LISTS module_targets)
            get_target_property(suffix ${module_target} DREAM_SHADER_SUFFIX)
            if(suffix)
                get_target_property(slang_target ${module_target} DREAM_SHADER_TARGET)
                get_target_property(profile ${module_target} DREAM_SHADER_PROFILE)
                list(APPEND formats "${suffix}|${slang_target}|${profile}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES formats)
    endif()
    set(${out_var} "${formats}" PARENT_SCOPE)
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
    set(module_libraries "")
    set(module_dirs "")

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
            list(APPEND module_targets dream_module_${interface}_${backend})
            list(APPEND module_libraries dream::module_${interface}_${backend})
            list(APPEND module_dirs "${DreamEngine_SOURCE_DIR}/modules/${interface}/${backend}")
        endforeach()
    endforeach()
    # Other build steps, such as shader compilation and APK packaging, adapt to the selection.
    set_target_properties(${target} PROPERTIES
        DREAM_MODULE_TARGETS "${module_targets}"
        DREAM_MODULE_DIRS "${module_dirs}")

    file(READ "${ARG_CONFIG}" DREAM_MODULE_CONFIG_TEXT)
    set(DREAM_MODULE_CONFIG_PATH "${ARG_CONFIG}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_module_registration.cpp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/module_registration.cpp.in" "${generated}"
        @ONLY)
    target_sources(${target} PRIVATE "${generated}")
    target_link_libraries(${target} PRIVATE dream::core ${module_libraries})
endfunction()
