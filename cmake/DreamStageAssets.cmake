# Merges a game's asset folders into one, as the game will see them. Run with cmake -P:
#
#   cmake -DSOURCES=<dir>|<dir>... -DDESTINATION=<dir> -P DreamStageAssets.cmake
#
# Later folders win when two have the same file.

if(NOT DEFINED SOURCES OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DreamStageAssets.cmake needs SOURCES and DESTINATION")
endif()

string(REPLACE "|" ";" source_dirs "${SOURCES}")
file(MAKE_DIRECTORY "${DESTINATION}")
foreach(source_dir IN LISTS source_dirs)
    if(NOT IS_DIRECTORY "${source_dir}")
        continue()
    endif()
    file(COPY "${source_dir}/" DESTINATION "${DESTINATION}")
endforeach()
