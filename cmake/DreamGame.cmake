# Builds a game for the current platform. See docs/adr/0007-platform-and-game-loop.md.
#
# Usage: dream_add_game(<name> SOURCES <files...> CONFIG <toml-file> [ASSETS <directory>]
#                       [APPLICATION_ID <id> DISPLAY_NAME <text>])
#
# On PC this creates an executable, with the assets copied next to it. On Android it creates
# libmain.so and, if APPLICATION_ID is given and ANDROID_HOME is set, a <name>_apk target.
function(dream_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CONFIG;ASSETS;APPLICATION_ID;DISPLAY_NAME" "SOURCES")

    if(ANDROID)
        set(native_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_native")
        add_library(${name} SHARED ${ARG_SOURCES})
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME main
            LIBRARY_OUTPUT_DIRECTORY "${native_dir}/${ANDROID_ABI}")
    else()
        add_executable(${name} ${ARG_SOURCES})
    endif()

    target_sources(${name} PRIVATE "${DreamEngine_SOURCE_DIR}/runtime/entry/game_entry.cpp")
    target_link_libraries(${name} PRIVATE dream::runtime)
    dream_configure_target(${name})
    dream_link_modules(${name} CONFIG "${ARG_CONFIG}")

    if(ARG_ASSETS)
        cmake_path(ABSOLUTE_PATH ARG_ASSETS NORMALIZE)
    endif()

    if(ANDROID)
        if(ARG_APPLICATION_ID)
            _dream_add_apk(${name} "${native_dir}" "${ARG_ASSETS}" "${ARG_APPLICATION_ID}"
                "${ARG_DISPLAY_NAME}")
        endif()
    elseif(ARG_ASSETS)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${ARG_ASSETS}"
                "$<TARGET_FILE_DIR:${name}>/assets"
            VERBATIM)
    endif()
endfunction()

function(_dream_add_apk name native_dir assets application_id display_name)
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
            "-PdreamAssetsDir=${assets}"
            "-PdreamJavaDirs=${java_dirs}"
            ${gradle_task}
        WORKING_DIRECTORY "${DreamEngine_SOURCE_DIR}/templates/android"
        COMMENT "Packaging ${name} as an APK in ${output_dir}/app/outputs/apk"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(${name}_apk ${name})
endfunction()
