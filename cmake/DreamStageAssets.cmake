# Merges a game's asset folders into one, as the game will see them. Run with cmake -P:
#
#   cmake -DSOURCES=<dir>|<dir>... -DDESTINATION=<dir> [-DCOOKED=ON] [-DLEAVE_OUT=<folder>|...]
#         -P DreamStageAssets.cmake
#
# Later folders win when two have the same file. With COOKED, PNG files in a folder named
# "textures" are left out, since the build cooked each into a .ktx2 that one of the folders holds.
# See docs/adr/0024-texture-cooking.md. PNG files under a LEAVE_OUT folder, relative to the assets'
# root, are left out too: the build packed them into an atlas (ADR 0052).

if(NOT DEFINED SOURCES OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DreamStageAssets.cmake needs SOURCES and DESTINATION")
endif()

string(REPLACE "|" ";" source_dirs "${SOURCES}")
string(REPLACE "|" ";" leave_out "${LEAVE_OUT}")
file(MAKE_DIRECTORY "${DESTINATION}")
foreach(source_dir IN LISTS source_dirs)
    if(NOT IS_DIRECTORY "${source_dir}")
        continue()
    endif()
    set(excluded "")
    if(COOKED)
        list(APPEND excluded REGEX "/textures/([^/]+/)*[^/]+\\.[Pp][Nn][Gg]$" EXCLUDE)
    endif()
    foreach(folder IN LISTS leave_out)
        # Under this folder, from the root of whichever source holds it.
        string(REPLACE "." "\\." pattern "/${folder}/")
        list(APPEND excluded REGEX "${pattern}([^/]+/)*[^/]+\\.[Pp][Nn][Gg]$" EXCLUDE)
    endforeach()
    file(COPY "${source_dir}/" DESTINATION "${DESTINATION}" ${excluded})
endforeach()
