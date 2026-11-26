# Reads which cookers a project's assets go through, from its TOML file (ADR 0043):
#
#     [cook]
#     textures = "ktx2_uastc"     # The default: the build's texture cooker for every platform.
#
#     [cook.android]
#     textures = "ktx2_etc1s"     # Optional: another one for one platform.
#
# Like DreamModuleConfig.cmake, it understands only this strict form, one quoted name per line, and
# stops with an error on anything else in these tables.
#
# Usage: dream_read_cook_config(<file> <platform> <out_textures>)
#
# Sets <out_textures> to the texture cooker's name, "ktx2_uastc" when the file names none.
function(dream_read_cook_config file platform out_textures)
    file(STRINGS "${file}" lines)
    set(table "")
    set(textures "ktx2_uastc")
    set(platform_textures "")
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(line MATCHES "^\\[cook\\][ \t]*(#.*)?$")
            set(table "cook")
        elseif(line MATCHES "^\\[cook\\.([a-z0-9_]+)\\][ \t]*(#.*)?$")
            set(table "cook.${CMAKE_MATCH_1}")
        elseif(line MATCHES "^\\[")
            set(table "")
        elseif(table STREQUAL "cook" OR table STREQUAL "cook.${platform}")
            if(NOT line MATCHES "^textures[ \t]*=[ \t]*\"([a-z0-9_]+)\"[ \t]*(#.*)?$")
                message(FATAL_ERROR "${file}:${line_number}: expected textures = \"<cooker>\" in "
                                    "[${table}], one quoted name of lower-case letters, digits and "
                                    "underscores.")
            endif()
            if(table STREQUAL "cook")
                set(textures "${CMAKE_MATCH_1}")
            else()
                set(platform_textures "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
    if(platform_textures)
        set(textures "${platform_textures}")
    endif()
    set(${out_textures} "${textures}" PARENT_SCOPE)
endfunction()

# Reads a project's bundles from its TOML file (ADR 0047): groups of assets shipped and loaded
# together, each named and given by the files it starts from, which bring everything they need:
#
#     [bundles.forest]
#     roots = ["assets:/scenes/forest.toml", "assets:/ui/forest_map.rml"]
#
# One list of quoted paths on one line, as with the tables above.
#
# Usage: dream_read_bundle_config(<file> <out_bundles>)
#
# Sets <out_bundles> to a list of "<name>=<root>,<root>", empty when the file has none.
function(dream_read_bundle_config file out_bundles)
    file(STRINGS "${file}" lines)
    set(table "")
    set(bundles "")
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(line MATCHES "^\\[bundles\\.([a-z0-9_]+)\\][ \t]*(#.*)?$")
            set(table "${CMAKE_MATCH_1}")
            if(table STREQUAL "base")
                message(FATAL_ERROR "${file}:${line_number}: \"base\" is the bundle of what no "
                                    "other holds, and can't be named")
            endif()
        elseif(line MATCHES "^\\[")
            set(table "")
        elseif(NOT table STREQUAL "")
            if(NOT line MATCHES "^roots[ \t]*=[ \t]*\\[(.*)\\][ \t]*(#.*)?$")
                message(FATAL_ERROR "${file}:${line_number}: expected roots = [\"assets:/...\"] "
                                    "in [bundles.${table}], on one line.")
            endif()
            string(REGEX MATCHALL "\"assets:/[^\"]+\"" roots "${CMAKE_MATCH_1}")
            if(NOT roots)
                message(FATAL_ERROR "${file}:${line_number}: bundle '${table}' needs at least one "
                                    "root, a quoted assets:/ path")
            endif()
            string(REPLACE "\"" "" roots "${roots}")
            list(JOIN roots "," joined)
            list(APPEND bundles "${table}=${joined}")
        endif()
    endforeach()
    set(${out_bundles} "${bundles}" PARENT_SCOPE)
endfunction()
