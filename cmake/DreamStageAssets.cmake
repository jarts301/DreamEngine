# Merges a game's asset folders into one, as the game will see them. Run with cmake -P:
#
#   cmake -DSOURCES=<dir>|<dir>... -DDESTINATION=<dir> [-DCOOKED=ON] -P DreamStageAssets.cmake
#
# Later folders win when two have the same file. With COOKED, PNG files in a folder named
# "textures" are left out, since the build cooked each into a .ktx2 that one of the folders holds.
# See docs/adr/0024-texture-cooking.md.

if(NOT DEFINED SOURCES OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DreamStageAssets.cmake needs SOURCES and DESTINATION")
endif()

string(REPLACE "|" ";" source_dirs "${SOURCES}")
file(MAKE_DIRECTORY "${DESTINATION}")
foreach(source_dir IN LISTS source_dirs)
    if(NOT IS_DIRECTORY "${source_dir}")
        continue()
    endif()
    if(COOKED)
        file(COPY "${source_dir}/" DESTINATION "${DESTINATION}"
            REGEX "/textures/([^/]+/)*[^/]+\\.[Pp][Nn][Gg]$" EXCLUDE)
    else()
        file(COPY "${source_dir}/" DESTINATION "${DESTINATION}")
    endif()
endforeach()
