# Builds a game for the current platform. See docs/adr/0007-platform-and-game-loop.md.
#
# Usage: dream_add_game(<name> SOURCES <files...> CONFIG <toml-file> [ASSETS <directories...>]
#                       [SHADERS <slang-files...>] [RENDERER]
#                       [APPLICATION_ID <id> DISPLAY_NAME <text>])
#
# RENDERER adds the shaders that runtime::Renderer needs.
#
# Each shader file must define vertex_main and fragment_main. They become the game assets
# assets:/shaders/<file>.vert and .frag, plus the suffix given by rhi::Device::shader_file_suffix.
#
# On PC this creates an executable, with the assets copied next to it. On Android it creates
# libmain.so and, if APPLICATION_ID is given and ANDROID_HOME is set, a <name>_apk target.
function(dream_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "RENDERER" "CONFIG;APPLICATION_ID;DISPLAY_NAME"
        "SOURCES;SHADERS;ASSETS")

    if(ARG_RENDERER)
        list(APPEND ARG_SHADERS
            "${DreamEngine_SOURCE_DIR}/runtime/shaders/forward.slang"
            "${DreamEngine_SOURCE_DIR}/runtime/shaders/debug_lines.slang")
    endif()

    if(ANDROID)
        set(native_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_native")
        add_library(${name} SHARED ${ARG_SOURCES})
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME main
            LIBRARY_OUTPUT_DIRECTORY "${native_dir}/${ANDROID_ABI}")
    else()
        add_executable(${name} ${ARG_SOURCES})
        # Each game gets its own folder, because its assets are copied next to it: games sharing a
        # folder would copy into the same assets folder at once, and fail at random.
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endif()

    target_sources(${name} PRIVATE "${DreamEngine_SOURCE_DIR}/runtime/entry/game_entry.cpp")
    target_link_libraries(${name} PRIVATE dream::runtime)
    dream_configure_target(${name})
    dream_link_modules(${name} CONFIG "${ARG_CONFIG}")

    # Each directory's contents are merged into the game's assets.
    set(asset_dirs "")
    foreach(asset_dir IN LISTS ARG_ASSETS)
        cmake_path(ABSOLUTE_PATH asset_dir NORMALIZE)
        list(APPEND asset_dirs "${asset_dir}")
    endforeach()
    if(ARG_SHADERS)
        set(generated_assets "${CMAKE_CURRENT_BINARY_DIR}/${name}_generated_assets")
        _dream_compile_shaders(${name} "${generated_assets}/shaders" ${ARG_SHADERS})
        list(APPEND asset_dirs "${generated_assets}")
    endif()

    if(ANDROID)
        if(ARG_APPLICATION_ID)
            _dream_add_apk(${name} "${native_dir}" "${asset_dirs}" "${ARG_APPLICATION_ID}"
                "${ARG_DISPLAY_NAME}")
        endif()
    else()
        foreach(asset_dir IN LISTS asset_dirs)
            add_custom_command(TARGET ${name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${asset_dir}"
                    "$<TARGET_FILE_DIR:${name}>/assets"
                VERBATIM)
        endforeach()
    endif()
endfunction()

# Compiles Slang shaders on the build machine, so a game ships only the compiled result.
function(_dream_compile_shaders target output_dir)
    # vcpkg installs this for the build machine, in a folder named after that machine's triplet.
    file(GLOB slang_dirs "${VCPKG_INSTALLED_DIR}/*/tools/shader-slang")
    find_program(DREAM_SLANGC slangc HINTS ${slang_dirs}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

    set(outputs "")
    foreach(source IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH source NORMALIZE)
        cmake_path(GET source STEM stem)

        foreach(stage IN ITEMS vertex fragment)
            string(SUBSTRING ${stage} 0 4 short_stage)
            set(output "${output_dir}/${stem}.${short_stage}.spv")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
                COMMAND "${DREAM_SLANGC}" "${source}"
                    -target spirv -profile spirv_1_3
                    -matrix-layout-column-major
                    -entry ${stage}_main -stage ${stage}
                    -o "${output}"
                DEPENDS "${source}"
                COMMENT "Compiling ${stem} ${stage} shader"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()

function(_dream_add_apk name native_dir asset_dirs application_id display_name)
    # The APK has to carry the C++ runtime that the game was linked against.
    find_file(DREAM_LIBCXX_SHARED libc++_shared.so
        PATHS "${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr/lib/aarch64-linux-android"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DREAM_LIBCXX_SHARED}"
            "${native_dir}/${ANDROID_ABI}/libc++_shared.so"
        VERBATIM)

    if(NOT DEFINED ENV{ANDROID_HOME})
        message(STATUS "ANDROID_HOME is not set, so there is no ${name}_apk target.")
        return()
    endif()

    # Backends can ship Java code that has to be compiled into the APK.
    get_target_property(module_dirs ${name} DREAM_MODULE_DIRS)
    set(java_dirs "")
    foreach(module_dir IN LISTS module_dirs)
        if(IS_DIRECTORY "${module_dir}/android/java")
            list(APPEND java_dirs "${module_dir}/android/java")
        endif()
    endforeach()
    list(JOIN java_dirs "," java_dirs)
    list(JOIN asset_dirs "," asset_dirs)

    if(CMAKE_HOST_WIN32)
        set(gradle "${DreamEngine_SOURCE_DIR}/templates/android/gradlew.bat")
    else()
        set(gradle sh "${DreamEngine_SOURCE_DIR}/templates/android/gradlew")
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(gradle_task assembleDebug)
    else()
        set(gradle_task assembleRelease)
    endif()

    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_apk")
    add_custom_target(${name}_apk
        COMMAND ${CMAKE_COMMAND} -E env "ANDROID_HOME=$ENV{ANDROID_HOME}"
            ${gradle} --no-daemon --project-cache-dir "${output_dir}/gradle-cache"
            "-PdreamBuildDir=${output_dir}"
            "-PdreamApplicationId=${application_id}"
            "-PdreamDisplayName=${display_name}"
            "-PdreamNativeDir=${native_dir}"
            "-PdreamAssetDirs=${asset_dirs}"
            "-PdreamJavaDirs=${java_dirs}"
            ${gradle_task}
        WORKING_DIRECTORY "${DreamEngine_SOURCE_DIR}/templates/android"
        COMMENT "Packaging ${name} as an APK in ${output_dir}/app/outputs/apk"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(${name}_apk ${name})
endfunction()
