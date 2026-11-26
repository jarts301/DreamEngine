# Gathers a game's release (ADR 0048) into one folder: what ships, and its symbols apart. Run with
# cmake -P by the <game>_release target:
#
#   cmake -DNAME=<game> -DVERSION=<version> -DPLATFORM=<platform> -DOUTPUT=<folder>
#         [-DPACKAGE=<package folder>] [-DFILES=<file>|<file>...] [-DSYMBOLS=<file>|<file>...]
#         -P DreamRelease.cmake
#
# It writes into OUTPUT, emptied first:
#
#   <game>-<version>-<platform>.zip            PACKAGE's contents, the PC game to hand out.
#   <game>-<version>-<platform>.<ext>          Each of FILES, such as the APK and the bundle.
#   <game>-<version>-<platform>-symbols.zip    SYMBOLS, the debug information of the release's
#                                              program or libraries, to read its crashes with.

foreach(required IN ITEMS NAME VERSION PLATFORM OUTPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "DreamRelease.cmake needs ${required}")
    endif()
endforeach()

set(base "${NAME}-${VERSION}-${PLATFORM}")
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# Zips everything inside `folder`, without the folder itself.
function(_dream_zip folder archive)
    file(GLOB entries RELATIVE "${folder}" "${folder}/*")
    if(NOT entries)
        message(FATAL_ERROR "There is nothing in ${folder} to release")
    endif()
    list(SORT entries)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip ${entries}
        WORKING_DIRECTORY "${folder}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not write ${archive}")
    endif()
endfunction()

if(DEFINED PACKAGE AND NOT PACKAGE STREQUAL "")
    _dream_zip("${PACKAGE}" "${OUTPUT}/${base}.zip")
    message(STATUS "Released ${OUTPUT}/${base}.zip")
endif()

if(DEFINED FILES AND NOT FILES STREQUAL "")
    string(REPLACE "|" ";" FILES "${FILES}")
    foreach(file IN LISTS FILES)
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "The release has no ${file}")
        endif()
        cmake_path(GET file EXTENSION LAST_ONLY extension)
        file(COPY_FILE "${file}" "${OUTPUT}/${base}${extension}")
        message(STATUS "Released ${OUTPUT}/${base}${extension}")
    endforeach()
endif()

if(DEFINED SYMBOLS AND NOT SYMBOLS STREQUAL "")
    string(REPLACE "|" ";" SYMBOLS "${SYMBOLS}")
    set(gathered "${OUTPUT}/symbols")
    file(MAKE_DIRECTORY "${gathered}")
    foreach(file IN LISTS SYMBOLS)
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "The release's symbols aren't in ${file}: is it a release build?")
        endif()
        file(COPY "${file}" DESTINATION "${gathered}")
    endforeach()
    _dream_zip("${gathered}" "${OUTPUT}/${base}-symbols.zip")
    file(REMOVE_RECURSE "${gathered}")
    message(STATUS "Released ${OUTPUT}/${base}-symbols.zip")
endif()
