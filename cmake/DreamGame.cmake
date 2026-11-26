# Builds a game for the current platform. See docs/adr/0007-platform-and-game-loop.md.
#
# Usage: dream_add_game(<name> SOURCES <files...> CONFIG <toml-file> [ASSETS <directories...>]
#                       [SHADERS <slang-files...>] [APPLICATION_ID <id> DISPLAY_NAME <text>]
#                       [VERSION <major.minor.patch>])
#
# The game's version (ADR 0048) is VERSION, or else the calling project's, as in
# project(my_game VERSION 1.4.2). A release build sets DREAM_GAME_VERSION from its tag, which wins
# over both. The game knows it as runtime::game_version(), its crash reports carry it, and so do its
# program's file properties on Windows and its APK's or bundle's versionName and versionCode.
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

include(${CMAKE_CURRENT_LIST_DIR}/DreamCookConfig.cmake)

# The version of every game in this build, from a release's tag, such as 1.4.2 for v1.4.2. Empty
# for each game's own.
set(DREAM_GAME_VERSION "" CACHE STRING "Every game's version, as a release sets it from its tag")

# Folders of texture cookers (ADR 0043), searched in order: the engine's own, then any a project
# adds, for a cooker of its own named in its [cook] table.
set(DREAM_TEXTURE_COOKER_DIRS "${CMAKE_CURRENT_LIST_DIR}/cookers/textures" CACHE STRING
    "Folders of texture cookers, searched in order")

function(dream_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CONFIG;APPLICATION_ID;DISPLAY_NAME;VERSION"
        "SOURCES;SHADERS;ASSETS")
    _dream_game_version(${name} "${ARG_VERSION}" version version_code)

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
    target_compile_definitions(${name} PRIVATE "DREAM_GAME_VERSION=\"${version}\"")
    set_target_properties(${name} PROPERTIES
        DREAM_GAME_VERSION "${version}"
        DREAM_GAME_VERSION_CODE "${version_code}")
    if(WIN32)
        _dream_version_resource(${name} "${version}" "${ARG_DISPLAY_NAME}")
    endif()
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
    # Modules built from the same files, such as forward and forward_plus (ADR 0037), share their
    # shaders, compiled once.
    list(REMOVE_DUPLICATES ARG_SHADERS)
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
    set(textures_cooked OFF)
    if(DREAM_COOK_TEXTURES)
        _dream_cook_textures(${name} "${generated_assets}" "${ARG_CONFIG}" textures_cooked
            ${asset_dirs})
    endif()
    if(DREAM_COOK_MODELS)
        _dream_cook_models(${name} "${generated_assets}" "${ARG_CONFIG}" ${asset_dirs})
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
    # After staging, the asset database: what each file is and needs, and its bundle (ADR 0047).
    # A file that names one the assets don't have fails the build here.
    dream_read_cook_config("${ARG_CONFIG}" "${DREAM_PLATFORM}" texture_cooker)
    if(NOT textures_cooked)
        set(texture_cooker none)
    endif()
    dream_read_bundle_config("${ARG_CONFIG}" bundles)
    # A packaged game reads its bundles from archives of their own, which only the bundles source
    # knows: the game must read through it.
    if(bundles)
        cmake_path(ABSOLUTE_PATH ARG_CONFIG NORMALIZE OUTPUT_VARIABLE config_path)
        dream_read_module_config("${config_path}" "${DREAM_PLATFORM}" modules)
        set(first_source "")
        if(modules_BACKENDS_asset_source)
            list(GET modules_BACKENDS_asset_source 0 first_source)
        endif()
        if(NOT first_source STREQUAL "bundles")
            message(FATAL_ERROR "${config_path} names bundles, which a game reads through the "
                "\"bundles\" asset source: list it first in [modules.${DREAM_PLATFORM}], as "
                "asset_source = [\"bundles\"].")
        endif()
    endif()
    # Joined with |, as a ; in a command's argument would split it.
    list(JOIN bundles "|" bundles)
    add_custom_target(${name}_assets
        COMMAND ${CMAKE_COMMAND} "-DSOURCES=${joined_dirs}" "-DDESTINATION=${staged_dir}"
            "-DCOOKED=${textures_cooked}"
            -P "${DreamEngine_SOURCE_DIR}/cmake/DreamStageAssets.cmake"
        COMMAND ${CMAKE_COMMAND} "-DASSETS=${staged_dir}" "-DPLATFORM=${DREAM_PLATFORM}"
            "-DTEXTURE_COOKER=${texture_cooker}" "-DBUNDLES=${bundles}"
            -P "${DreamEngine_SOURCE_DIR}/cmake/DreamAssetDatabase.cmake"
        VERBATIM)
    foreach(made IN ITEMS shaders textures models)
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
                add_dependencies(${name}_aab ${name}_assets)
                add_dependencies(${name}_release ${name}_assets)
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

            # The release (ADR 0048): the package zipped, and the program's symbols apart.
            get_target_property(version ${name} DREAM_GAME_VERSION)
            set(symbols "")
            if(MSVC AND DREAM_RELEASE_SYMBOLS)
                set(symbols "$<TARGET_PDB_FILE:${name}>")
            endif()
            add_custom_target(${name}_release
                COMMAND ${CMAKE_COMMAND} "-DNAME=${name}" "-DVERSION=${version}"
                    "-DPLATFORM=${DREAM_PLATFORM}"
                    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}_release"
                    "-DPACKAGE=${package_dir}" "-DSYMBOLS=${symbols}"
                    -P "${DreamEngine_SOURCE_DIR}/cmake/DreamRelease.cmake"
                VERBATIM)
            add_dependencies(${name}_release ${name}_package)
        endif()
    endif()
endfunction()

# The game's version: DREAM_GAME_VERSION, a release's, or else `given`, or else the calling
# project's. Three whole numbers, which Android also needs as one, its versionCode: major * 10000 +
# minor * 100 + patch, so minor and patch stay below 100.
function(_dream_game_version name given out_version out_code)
    if(NOT DREAM_GAME_VERSION STREQUAL "")
        set(version "${DREAM_GAME_VERSION}")
    elseif(NOT given STREQUAL "")
        set(version "${given}")
    elseif(NOT PROJECT_VERSION STREQUAL "")
        set(version "${PROJECT_VERSION}")
    else()
        set(version "0.0.0")
    endif()
    if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "${name}'s version '${version}' isn't <major>.<minor>.<patch>, three "
                            "whole numbers, such as 1.4.2")
    endif()
    set(major "${CMAKE_MATCH_1}")
    set(minor "${CMAKE_MATCH_2}")
    set(patch "${CMAKE_MATCH_3}")
    if(minor GREATER 99 OR patch GREATER 99 OR major GREATER 20000)
        message(FATAL_ERROR "${name}'s version '${version}' can't be an Android versionCode: "
                            "minor and patch go up to 99, and major to 20000")
    endif()
    math(EXPR code "${major} * 10000 + ${minor} * 100 + ${patch}")
    set(${out_version} "${version}" PARENT_SCOPE)
    set(${out_code} "${code}" PARENT_SCOPE)
endfunction()

# The version shown in a Windows program's file properties.
function(_dream_version_resource name version display_name)
    if(display_name STREQUAL "")
        set(display_name "${name}")
    endif()
    string(REPLACE "\"" "\"\"" display_name "${display_name}")
    string(REPLACE "." "," numbers "${version}")
    set(resource "${CMAKE_CURRENT_BINARY_DIR}/${name}_version.rc")
    file(CONFIGURE OUTPUT "${resource}" CONTENT "1 VERSIONINFO
FILEVERSION ${numbers},0
PRODUCTVERSION ${numbers},0
FILEOS 0x40004
FILETYPE 0x1
BEGIN
    BLOCK \"StringFileInfo\"
    BEGIN
        BLOCK \"040904B0\"
        BEGIN
            VALUE \"FileDescription\", \"${display_name}\"
            VALUE \"ProductName\", \"${display_name}\"
            VALUE \"FileVersion\", \"${version}\"
            VALUE \"ProductVersion\", \"${version}\"
            VALUE \"InternalName\", \"${name}\"
            VALUE \"OriginalFilename\", \"${name}.exe\"
        END
    END
    BLOCK \"VarFileInfo\"
    BEGIN
        VALUE \"Translation\", 0x409, 1200
    END
END
" @ONLY)
    target_sources(${name} PRIVATE "${resource}")
endfunction()

# Cooks every PNG in a folder named "textures" among the asset folders into a KTX2 file of the same
# name in `output_dir`, with UASTC data and every mip level, compressed with zstd. The game loads it
# in place of the PNG and transcodes it for its GPU. Cooking runs on the build machine, with the
# ktx tool vcpkg builds for it. See docs/adr/0024-texture-cooking.md.
#
# Textures are colors, in sRGB, unless their names end in one of the suffixes below, which mark
# linear data such as normal maps, whose smaller levels must be made without sRGB's curve. Normal
# maps' smaller levels are also made unit length again. See docs/guides/materials.md.
function(_dream_cook_textures target output_dir config out_cooked)
    # Here rather than beside the function, so a game whose build adds the engine as a
    # subdirectory, as find_package(DreamEngine) does, sees them too.
    set(_DREAM_NORMAL_TEXTURE_SUFFIXES "_normal" "_normals" "_nrm")
    set(_DREAM_LINEAR_TEXTURE_SUFFIXES "_orm" "_metallic_roughness" "_mr" "_roughness" "_metallic"
        "_occlusion" "_ao" "_linear")
    # The project's texture cooker for this platform (ADR 0043): a file in one of
    # DREAM_TEXTURE_COOKER_DIRS, the engine's own first, that defines how each image is cooked.
    dream_read_cook_config("${config}" "${DREAM_PLATFORM}" cooker)
    set(cooker_file "")
    foreach(dir IN LISTS DREAM_TEXTURE_COOKER_DIRS)
        if(EXISTS "${dir}/${cooker}.cmake")
            set(cooker_file "${dir}/${cooker}.cmake")
            break()
        endif()
    endforeach()
    if(NOT cooker_file)
        message(FATAL_ERROR "${config} names the texture cooker \"${cooker}\", which none of "
                            "these folders has: ${DREAM_TEXTURE_COOKER_DIRS}")
    endif()
    unset(DREAM_TEXTURE_COOKER_NONE)
    unset(DREAM_TEXTURE_COOKER_DECODER)
    include("${cooker_file}")
    if(DREAM_TEXTURE_COOKER_NONE)
        # Textures a cooker made before are removed, or they would be staged and used.
        file(GLOB_RECURSE stale "${output_dir}/*.ktx2")
        if(stale)
            file(REMOVE ${stale})
        endif()
        set(${out_cooked} OFF PARENT_SCOPE)
        return()
    endif()
    if(NOT COMMAND dream_texture_cooker_${cooker})
        message(FATAL_ERROR "${cooker_file} doesn't define dream_texture_cooker_${cooker}()")
    endif()
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
            cmake_language(CALL dream_texture_cooker_${cooker} ${kind} arguments)
            set(output "${output_dir}/${relative}")
            cmake_path(GET output PARENT_PATH output_parent)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${output_parent}"
                COMMAND "${DREAM_KTX}" create ${arguments} "${source}" "${output}"
                DEPENDS "${source}" "${cooker_file}" "${config}"
                COMMENT "Cooking ${relative} (${cooker})"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endforeach()
    # The game must be able to read what the cooker made.
    if(outputs AND DREAM_TEXTURE_COOKER_DECODER)
        cmake_path(ABSOLUTE_PATH config NORMALIZE OUTPUT_VARIABLE config_path)
        dream_read_module_config("${config_path}" "${DREAM_PLATFORM}" modules)
        set(first_decoder "")
        if(modules_BACKENDS_texture_decoder)
            list(GET modules_BACKENDS_texture_decoder 0 first_decoder)
        endif()
        if(NOT first_decoder STREQUAL DREAM_TEXTURE_COOKER_DECODER)
            message(FATAL_ERROR "${config_path} cooks textures with \"${cooker}\", which the "
                "\"${DREAM_TEXTURE_COOKER_DECODER}\" texture decoder reads, so "
                "[modules.${DREAM_PLATFORM}] must list it first: "
                "texture_decoder = [\"${DREAM_TEXTURE_COOKER_DECODER}\"]. "
                "Or cook no textures: [cook] textures = \"none\".")
        endif()
    endif()
    if(outputs)
        add_custom_target(${target}_textures DEPENDS ${outputs})
    endif()
    set(${out_cooked} ON PARENT_SCOPE)
endfunction()

# Makes every .glb model's levels of detail with the model cook tool, into a .lods file of the same
# name, which the runtime reads in place of making them as the model loads. PC builds build the
# tool themselves; cross-compiled builds, such as Android's, take it from the dream-model-cook port,
# which vcpkg builds for the build machine. Without the tool, nothing is cooked, and the runtime
# makes the versions as models load. See docs/adr/0042-cooked-levels-of-detail.md.
function(_dream_cook_models target output_dir config)
    if(TARGET dream_model_cook)
        set(cook "$<TARGET_FILE:dream_model_cook>")
        set(cook_depends dream_model_cook)
    else()
        file(GLOB cook_dirs "${VCPKG_INSTALLED_DIR}/*/tools/dream-model-cook")
        find_program(DREAM_MODEL_COOK dream_model_cook HINTS ${cook_dirs} NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH)
        if(NOT DREAM_MODEL_COOK)
            message(STATUS "${target}: no model cook tool, so levels of detail are made as models "
                           "load")
            return()
        endif()
        set(cook "${DREAM_MODEL_COOK}")
        set(cook_depends "${DREAM_MODEL_COOK}")
    endif()
    if(ANDROID)
        set(platform android)
    elseif(WIN32)
        set(platform windows)
    else()
        set(platform linux)
    endif()
    cmake_path(ABSOLUTE_PATH config NORMALIZE)

    set(outputs "")
    foreach(asset_dir IN LISTS ARGN)
        file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${asset_dir}/*.glb")
        foreach(source IN LISTS sources)
            file(RELATIVE_PATH relative "${asset_dir}" "${source}")
            string(REGEX REPLACE "\.glb$" ".lods" relative "${relative}")
            set(output "${output_dir}/${relative}")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${cook}" "${source}" "${output}" "${config}" ${platform}
                DEPENDS "${source}" "${config}" ${cook_depends}
                COMMENT "Cooking levels of detail for ${relative}"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endforeach()
    if(outputs)
        add_custom_target(${target}_models DEPENDS ${outputs})
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
        set(variant Debug)
        # Debug libraries keep their symbols in the APK, for debugging on the device.
        set(keep_symbols true)
    else()
        set(variant Release)
        # Release libraries go to Gradle with their symbols, which it strips from what ships and
        # puts into the bundle for Google Play (ADR 0048).
        set(keep_symbols false)
    endif()

    dream_module_android_permissions(${name} permissions)
    list(JOIN permissions "," permissions)
    get_target_property(version ${name} DREAM_GAME_VERSION)
    get_target_property(version_code ${name} DREAM_GAME_VERSION_CODE)

    # Gradle strips with the NDK the game was built with, which it takes only by its exact version.
    set(ndk_version "")
    if(EXISTS "${CMAKE_ANDROID_NDK}/source.properties")
        file(STRINGS "${CMAKE_ANDROID_NDK}/source.properties" revision REGEX "^Pkg\\.Revision")
        string(REGEX REPLACE "^Pkg\\.Revision[ \t]*=[ \t]*" "" ndk_version "${revision}")
    endif()

    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_apk")
    set(gradle_properties
        "-PdreamBuildDir=${output_dir}"
        "-PdreamApplicationId=${application_id}"
        "-PdreamDisplayName=${display_name}"
        "-PdreamNativeDir=${native_dir}"
        "-PdreamAssetDirs=${asset_dirs}"
        "-PdreamJavaDirs=${java_dirs}"
        "-PdreamPermissions=${permissions}"
        "-PdreamVersionName=${version}"
        "-PdreamVersionCode=${version_code}"
        "-PdreamKeepSymbols=${keep_symbols}"
        "-PdreamNdkDir=${CMAKE_ANDROID_NDK}"
        "-PdreamNdkVersion=${ndk_version}")
    add_custom_target(${name}_apk
        COMMAND ${CMAKE_COMMAND} -E env "ANDROID_HOME=$ENV{ANDROID_HOME}"
            ${gradle} --no-daemon --project-cache-dir "${output_dir}/gradle-cache"
            ${gradle_properties} assemble${variant}
        WORKING_DIRECTORY "${DreamEngine_SOURCE_DIR}/templates/android"
        COMMENT "Packaging ${name} as an APK in ${output_dir}/app/outputs/apk"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(${name}_apk ${name})
    # An Android App Bundle, as Google Play takes (ADR 0048).
    add_custom_target(${name}_aab
        COMMAND ${CMAKE_COMMAND} -E env "ANDROID_HOME=$ENV{ANDROID_HOME}"
            ${gradle} --no-daemon --project-cache-dir "${output_dir}/gradle-cache"
            ${gradle_properties} bundle${variant}
        WORKING_DIRECTORY "${DreamEngine_SOURCE_DIR}/templates/android"
        COMMENT "Packaging ${name} as an Android App Bundle in ${output_dir}/app/outputs/bundle"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(${name}_aab ${name})

    # The release (ADR 0048): the APK and the bundle, from one Gradle run, so two don't share its
    # folders at once, and the library's symbols, from before Gradle strips it.
    string(TOLOWER "${variant}" lower_variant)
    set(outputs "${output_dir}/app/outputs")
    set(apk "${outputs}/apk/${lower_variant}/app-${lower_variant}.apk")
    set(bundle "${outputs}/bundle/${lower_variant}/app-${lower_variant}.aab")
    add_custom_target(${name}_release
        COMMAND ${CMAKE_COMMAND} -E env "ANDROID_HOME=$ENV{ANDROID_HOME}"
            ${gradle} --no-daemon --project-cache-dir "${output_dir}/gradle-cache"
            ${gradle_properties} assemble${variant} bundle${variant}
        COMMAND ${CMAKE_COMMAND} "-DNAME=${name}" "-DVERSION=${version}" -DPLATFORM=android
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}_release"
            "-DFILES=${apk}|${bundle}"
            "-DSYMBOLS=$<TARGET_FILE:${name}>"
            -P "${DreamEngine_SOURCE_DIR}/cmake/DreamRelease.cmake"
        WORKING_DIRECTORY "${DreamEngine_SOURCE_DIR}/templates/android"
        COMMENT "Releasing ${name} ${version} in ${CMAKE_CURRENT_BINARY_DIR}/${name}_release"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(${name}_release ${name})
endfunction()
