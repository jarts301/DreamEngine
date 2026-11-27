# Tiled's JSON maps and tilesets (ADR 0050), read with yyjson.
# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(yyjson CONFIG REQUIRED GLOBAL)

dream_add_module(tile_map_format tiled
    SOURCES src/tiled_reader.cpp
    LIBRARIES yyjson::yyjson
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_tile_map_format_tiled_tests
        SOURCES
            tests/test_main.cpp
            tests/tiled_reader_test.cpp
        LIBRARIES
            dream::module_tile_map_format_tiled
    )
endif()
