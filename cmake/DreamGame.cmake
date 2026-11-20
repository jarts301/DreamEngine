# Builds a game. See docs/adr/0007-platform-and-game-loop.md and, for this branch's Linux-only,
# offline build, docs/adr/0039-offline-linux-web-build.md.
#
# Usage: dream_add_game(<name> SOURCES <files...> CONFIG <toml-file> [ASSETS <directories...>])
#
# This creates an executable in a folder of its own, with the assets copied next to it. Textures
# ship as their PNG files: this build has no texture cooking and no shader compiler, because the
# software renderer draws on the CPU with no shaders to compile.
function(dream_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CONFIG" "SOURCES;ASSETS")

    add_executable(${name} ${ARG_SOURCES})
    # Each game gets its own folder, because its assets are copied next to it: games sharing a
    # folder would copy into the same assets folder at once, and fail at random.
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")

    target_sources(${name} PRIVATE "${DreamEngine_SOURCE_DIR}/runtime/entry/game_entry.cpp")
    target_link_libraries(${name} PRIVATE dream::runtime)
    dream_configure_target(${name})
    dream_link_modules(${name} CONFIG "${ARG_CONFIG}")

    # A game that lists a profiler counts its heap allocations, which costs a little on every one,
    # so games that don't list one are left alone. See docs/adr/0016-profiling-resources.md.
    get_target_property(module_targets ${name} DREAM_MODULE_TARGETS)
    list(FILTER module_targets INCLUDE REGEX "^dream_module_profiler_")
    if(module_targets)
        target_sources(${name} PRIVATE "${DreamEngine_SOURCE_DIR}/runtime/entry/heap_counting.cpp")
    endif()

    # Each directory's contents are merged into the game's assets, later ones winning.
    set(asset_dirs "")
    foreach(asset_dir IN LISTS ARG_ASSETS)
        cmake_path(ABSOLUTE_PATH asset_dir NORMALIZE)
        list(APPEND asset_dirs "${asset_dir}")
    endforeach()

    # A target of its own that runs on every build, so a changed asset is copied even when the
    # program itself is up to date. It fills the assets folder next to the program, naming the
    # program's folder directly, since naming the program would make each target wait for the
    # other.
    set(staged_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        string(APPEND staged_dir "/$<CONFIG>")
    endif()
    string(APPEND staged_dir "/assets")
    list(JOIN asset_dirs "|" joined_dirs)
    add_custom_target(${name}_assets
        COMMAND ${CMAKE_COMMAND} "-DSOURCES=${joined_dirs}" "-DDESTINATION=${staged_dir}"
            -P "${DreamEngine_SOURCE_DIR}/cmake/DreamStageAssets.cmake"
        VERBATIM)
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

    # The hub (tools/hub) lists the games it can start from this file, one line per game:
    # <name>|<program>|<title>.
    if(NOT ARG_CONFIG MATCHES "(^|/)tests/")
        file(READ "${ARG_CONFIG}" config_text)
        set(title "${name}")
        if(config_text MATCHES "\ntitle = \"([^\"]*)\"")
            set(title "${CMAKE_MATCH_1}")
        endif()
        set_property(GLOBAL APPEND PROPERTY DREAM_HUB_GAMES
            "${name}|${CMAKE_CURRENT_BINARY_DIR}/${name}/${name}|${title}")
    endif()
endfunction()
