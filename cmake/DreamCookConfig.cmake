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
