# Builds a game for the current platform. See docs/adr/0007-platform-and-game-loop.md.
#
# Usage: dream_add_game(<name> SOURCES <files...> CONFIG <toml-file> [ASSETS <directories...>]
#                       [SHADERS <slang-files...>] [APPLICATION_ID <id> DISPLAY_NAME <text>])
#
# The shaders of the modules the game links, such as its renderer's, are compiled with its own.
#
# Each shader file must define vertex_main and fragment_main. They become the game assets
# assets:/shaders/<file>.vert and .frag, plus the suffix given by rhi::Device::shader_file_suffix.
# They are compiled once for each format that the game's graphics backends load, so the module
# lists in CONFIG decide the formats; see dream_add_module.
#
# On PC this creates an executable, with the assets copied next to it. On Android it creates
# libmain.so and, if APPLICATION_ID is given and ANDROID_HOME is set, a <name>_apk target.
function(dream_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CONFIG;APPLICATION_ID;DISPLAY_NAME"
        "SOURCES;SHADERS;ASSETS")

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

    # A game that lists a profiler counts its heap allocations, which costs a little on every one,
    # so games that don't list one are left alone. See docs/adr/0016-profiling-resources.md.
    get_target_property(module_targets ${name} DREAM_MODULE_TARGETS)
    foreach(module_target IN LISTS module_targets)
        get_target_property(module_shaders ${module_target} DREAM_MODULE_SHADERS)
        if(module_shaders)
            list(APPEND ARG_SHADERS ${module_shaders})
        endif()
    endforeach()
    list(FILTER module_targets INCLUDE REGEX "^dream_module_profiler_")
    if(module_targets)
        target_sources(${name} PRIVATE "${DreamEngine_SOURCE_DIR}/runtime/entry/heap_counting.cpp")
    endif()

    # Each directory's contents are merged into the game's assets, and what the build makes from
    # them, compiled shaders and cooked textures, comes last.
    set(asset_dirs "")
    foreach(asset_dir IN LISTS ARG_ASSETS)
        cmake_path(ABSOLUTE_PATH asset_dir NORMALIZE)
        list(APPEND asset_dirs "${asset_dir}")
    endforeach()
    set(generated_assets "${CMAKE_CURRENT_BINARY_DIR}/${name}_generated_assets")
    if(ARG_SHADERS)
        dream_module_shader_formats(${name} shader_formats)
        if(shader_formats)
            _dream_compile_shaders(${name} "${generated_assets}/shaders" "${shader_formats}"
                ${ARG_SHADERS})
        else()
            message(STATUS "${name} has shaders, but none of its modules loads compiled shaders, "
                           "so they are not compiled.")
        endif()
    endif()
    if(DREAM_COOK_TEXTURES)
        _dream_cook_textures(${name} "${generated_assets}" ${asset_dirs})
    endif()
    list(APPEND asset_dirs "${generated_assets}")

    # A target of its own that runs on every build, so a changed asset is copied even when the
    # program itself is up to date. On PC it fills the assets folder next to the program, naming
    # the program's folder directly, since naming the program would make each target wait for the
    # other. On Android the APK packages the staged folder.
    if(ANDROID)
        set(staged_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_staged_assets")
    else()
        set(staged_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(multi_config)
            string(APPEND staged_dir "/$<CONFIG>")
        endif()
        string(APPEND staged_dir "/assets")
    endif()
    list(JOIN asset_dirs "|" joined_dirs)
    add_custom_target(${name}_assets
        COMMAND ${CMAKE_COMMAND} "-DSOURCES=${joined_dirs}" "-DDESTINATION=${staged_dir}"
            "-DCOOKED=${DREAM_COOK_TEXTURES}"
            -P "${DreamEngine_SOURCE_DIR}/cmake/DreamStageAssets.cmake"
        VERBATIM)
    foreach(made IN ITEMS shaders textures)
        if(TARGET ${name}_${made})
            add_dependencies(${name}_assets ${name}_${made})
        endif()
    endforeach()

    if(ANDROID)
        if(ARG_APPLICATION_ID)
            _dream_add_apk(${name} "${native_dir}" "${staged_dir}" "${ARG_APPLICATION_ID}"
                "${ARG_DISPLAY_NAME}")
            if(TARGET ${name}_apk)
                add_dependencies(${name}_apk ${name}_assets)
            endif()
        endif()
    else()
        add_dependencies(${name} ${name}_assets)

        # The game ready to hand out: the program, and its assets packed into assets.pak, which it
        # reads in place of the assets folder. See docs/adr/0024-texture-cooking.md.
        if(TARGET dream_pack)
            set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_package")
            add_custom_target(${name}_package
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${package_dir}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${package_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${name}>" "${package_dir}"
                COMMAND dream_pack "${staged_dir}" "${package_dir}/assets.pak"
                COMMENT "Packaging ${name} in ${package_dir}"
                VERBATIM)
            add_dependencies(${name}_package ${name} dream_pack)
        endif()
    endif()
endfunction()

# Cooks every PNG in a folder named "textures" among the asset folders into a KTX2 file of the same
# name in `output_dir`, with UASTC data and every mip level, compressed with zstd. The game loads it
# in place of the PNG and transcodes it for its GPU. Cooking runs on the build machine, with the
# ktx tool vcpkg builds for it. See docs/adr/0024-texture-cooking.md.
#
# Textures are colors, in sRGB, unless their names end in one of the suffixes below, which mark
# linear data such as normal maps, whose smaller levels must be made without sRGB's curve. Normal
# maps' smaller levels are also made unit length again. See docs/guides/materials.md.
set(_DREAM_NORMAL_TEXTURE_SUFFIXES "_normal" "_normals" "_nrm")
set(_DREAM_LINEAR_TEXTURE_SUFFIXES "_orm" "_metallic_roughness" "_mr" "_roughness" "_metallic"
    "_occlusion" "_ao" "_linear")
function(_dream_cook_textures target output_dir)
    file(GLOB ktx_dirs "${VCPKG_INSTALLED_DIR}/*/tools/ktx")
    find_program(DREAM_KTX ktx HINTS ${ktx_dirs} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

    set(outputs "")
    foreach(asset_dir IN LISTS ARGN)
        file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${asset_dir}/*.png" "${asset_dir}/*.PNG")
        foreach(source IN LISTS sources)
            file(RELATIVE_PATH relative "${asset_dir}" "${source}")
            if(NOT "/${relative}" MATCHES "/textures/")
                continue()
            endif()
            string(REGEX REPLACE "\.[Pp][Nn][Gg]$" ".ktx2" relative "${relative}")
            cmake_path(GET source STEM LAST_ONLY stem)
            string(TOLOWER "${stem}" stem)
            set(kind color)
            foreach(suffix IN LISTS _DREAM_NORMAL_TEXTURE_SUFFIXES _DREAM_LINEAR_TEXTURE_SUFFIXES)
                if(stem MATCHES "${suffix}$")
                    list(FIND _DREAM_NORMAL_TEXTURE_SUFFIXES "${suffix}" normal_index)
                    if(normal_index GREATER -1)
                        set(kind normal)
                    else()
                        set(kind linear)
                    endif()
                    break()
                endif()
            endforeach()
            if(kind STREQUAL "color")
                set(color_options --format R8G8B8A8_SRGB --assign-tf srgb)
            elseif(kind STREQUAL "normal")
                set(color_options --format R8G8B8A8_UNORM --assign-tf linear --normalize)
            else()
                set(color_options --format R8G8B8A8_UNORM --assign-tf linear)
            endif()
            set(output "${output_dir}/${relative}")
            cmake_path(GET output PARENT_PATH output_parent)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${output_parent}"
                COMMAND "${DREAM_KTX}" create ${color_options} --encode uastc
                    --uastc-quality 2 --zstd 18 --generate-mipmap
                    "${source}" "${output}"
                DEPENDS "${source}"
                COMMENT "Cooking ${relative}"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endforeach()
    if(outputs)
        add_custom_target(${target}_textures DEPENDS ${outputs})
    endif()
endfunction()

# SPIRV-Cross, which vcpkg installs for the build machine like slangc.
macro(_dream_find_spirv_cross)
    file(GLOB spirv_cross_dirs "${VCPKG_INSTALLED_DIR}/*/tools/spirv-cross")
    find_program(DREAM_SPIRV_CROSS spirv-cross HINTS ${spirv_cross_dirs}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endmacro()

# Compiles Slang shaders on the build machine, so a game ships only the compiled result. `formats`
# is the list from dream_module_shader_formats, and every shader is compiled once per format.
function(_dream_compile_shaders target output_dir formats)
    # vcpkg installs this for the build machine, in a folder named after that machine's triplet.
    file(GLOB slang_dirs "${VCPKG_INSTALLED_DIR}/*/tools/shader-slang")
    find_program(DREAM_SLANGC slangc HINTS ${slang_dirs}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

    # Shaders give push constants a DirectX register next to [[vk::push_constant]] (ADR 0013). When
    # compiling SPIR-V, Slang also reads that register as a Vulkan binding, as DXC would, and warns
    # that it overlaps the texture's binding even though a push constant gets no binding at all.
    # Warning 39001 is that false alarm, so it is turned off.
    set(common_options -matrix-layout-column-major -warnings-disable 39001)

    # A shader may #include another, such as forward_masked.slang including forward.slang, or a file
    # that is only included, such as skinning.slang, so each is compiled again when any Slang file
    # beside the target's shaders changes. There are only a few.
    set(all_sources "")
    foreach(source IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH source NORMALIZE)
        cmake_path(GET source PARENT_PATH source_dir)
        file(GLOB neighbours CONFIGURE_DEPENDS "${source_dir}/*.slang")
        list(APPEND all_sources "${source}" ${neighbours})
    endforeach()
    list(REMOVE_DUPLICATES all_sources)

    set(outputs "")
    foreach(format IN LISTS formats)
        string(REPLACE "|" ";" format "${format}")
        list(GET format 0 suffix)
        list(GET format 1 slang_target)
        list(GET format 2 profile)

        foreach(source IN LISTS ARGN)
            cmake_path(ABSOLUTE_PATH source NORMALIZE)
            cmake_path(GET source STEM stem)

            foreach(stage IN ITEMS vertex fragment)
                string(SUBSTRING ${stage} 0 4 short_stage)
                set(output "${output_dir}/${stem}.${short_stage}${suffix}")
                if(slang_target STREQUAL "glsl" OR slang_target STREQUAL "essl")
                    # Through SPIR-V and SPIRV-Cross, for the OpenGL backends (ADR 0026). The
                    # profile is the GLSL version.
                    _dream_find_spirv_cross()
                    set(script "${DreamEngine_SOURCE_DIR}/cmake/DreamCrossCompileShader.cmake")
                    add_custom_command(
                        OUTPUT "${output}"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
                        COMMAND ${CMAKE_COMMAND} -DSLANGC=${DREAM_SLANGC}
                            -DSPIRV_CROSS=${DREAM_SPIRV_CROSS} -DSOURCE=${source}
                            -DSTAGE=${stage} -DOUTPUT=${output} -DLANGUAGE=${slang_target}
                            -DVERSION=${profile} -P "${script}"
                        DEPENDS ${all_sources} "${script}"
                        COMMENT "Compiling ${stem} ${stage} shader to ${suffix}"
                        VERBATIM)
                else()
                    add_custom_command(
                        OUTPUT "${output}"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
                        COMMAND "${DREAM_SLANGC}" "${source}"
                            -target ${slang_target} -profile ${profile}
                            ${common_options}
                            -entry ${stage}_main -stage ${stage}
                            -o "${output}"
                        DEPENDS ${all_sources}
                        COMMENT "Compiling ${stem} ${stage} shader to ${suffix}"
                        VERBATIM)
                endif()
                list(APPEND outputs "${output}")
            endforeach()
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
