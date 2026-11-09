# Reads the module lists from a project's TOML file so CMake knows what to compile.
#
# CMake has no TOML parser, so this understands only one strict form of the [modules.<platform>]
# tables and stops with an error on anything else. It can't silently read those tables differently
# from the real parser that loads the same file at run time. Other tables are ignored here.
#
#     [modules.windows]
#     physics = ["jolt", "bullet"]
#
# Usage: dream_read_module_config(<file> <platform> <prefix>)
#
# Sets in the caller's scope:
#   <prefix>_FOUND               TRUE if the file has a table for <platform>
#   <prefix>_INTERFACES          the interfaces listed for <platform>
#   <prefix>_BACKENDS_<name>     the backends listed for interface <name>, in order

function(dream_read_module_config file platform prefix)
    file(READ "${file}" text)
    string(REPLACE "\r" "" text "${text}")

    set(found FALSE)
    set(interfaces "")
    set(line_number 0)
    # One of: root, other, modules.
    set(table root)
    set(table_platform "")

    # Walks the lines by searching for newlines, because a CMake list would mangle the brackets and
    # semicolons that TOML text can contain.
    while(NOT text STREQUAL "")
        string(FIND "${text}" "\n" line_end)
        if(line_end EQUAL -1)
            set(line "${text}")
            set(text "")
        else()
            string(SUBSTRING "${text}" 0 ${line_end} line)
            math(EXPR next_line "${line_end} + 1")
            string(SUBSTRING "${text}" ${next_line} -1 text)
        endif()
        math(EXPR line_number "${line_number} + 1")
        string(STRIP "${line}" line)

        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()

        set(location "${file}:${line_number}")

        if(line MATCHES "^\\[+[ \t\"']*modules[] \t\"'.]")
            if(NOT line MATCHES "^\\[modules\\.([a-z0-9_]+)\\][ \t]*(#.*)?$")
                message(FATAL_ERROR "${location}: expected a table named [modules.<platform>], "
                                    "using lower-case letters, digits and underscores.")
            endif()
            set(table modules)
            set(table_platform "${CMAKE_MATCH_1}")
            if(table_platform STREQUAL platform)
                set(found TRUE)
            endif()
        elseif(line MATCHES "^\\[")
            set(table other)
        elseif(table STREQUAL "root" AND line MATCHES "^[\"']?modules[\"']?[ \t]*[.=]")
            message(FATAL_ERROR "${location}: write module lists under a [modules.<platform>] "
                                "table, not as dotted keys.")
        elseif(table STREQUAL "modules")
            if(NOT line MATCHES "^([a-z0-9_]+)[ \t]*=[ \t]*\\[([^]#]*)\\][ \t]*(#.*)?$")
                message(FATAL_ERROR "${location}: expected interface = [\"backend\", ...] on one "
                                    "line.")
            endif()
            set(interface "${CMAKE_MATCH_1}")
            set(body "${CMAKE_MATCH_2}")

            string(REGEX REPLACE "\"[a-z0-9_]+\"" "" leftover "${body}")
            string(REGEX REPLACE "[ \t,]" "" leftover "${leftover}")
            if(NOT leftover STREQUAL "")
                message(FATAL_ERROR "${location}: backend names must be quoted and use lower-case "
                                    "letters, digits and underscores.")
            endif()

            if(table_platform STREQUAL platform)
                string(REGEX MATCHALL "\"[a-z0-9_]+\"" backends "${body}")
                string(REPLACE "\"" "" backends "${backends}")
                list(REMOVE_DUPLICATES backends)
                list(APPEND interfaces "${interface}")
                set(${prefix}_BACKENDS_${interface} "${backends}" PARENT_SCOPE)
            endif()
        endif()
    endwhile()

    set(${prefix}_FOUND ${found} PARENT_SCOPE)
    set(${prefix}_INTERFACES "${interfaces}" PARENT_SCOPE)
endfunction()
