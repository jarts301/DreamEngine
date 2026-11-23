# The version find_package(DreamEngine <version>) checks, read from the engine's CMakeLists.txt.
# While the engine is at 0.x, each minor version may break games, so only the same minor version
# matches; from 1.0, any later version with the same major number does.

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" _dream_project_line
    REGEX "^project\\(DreamEngine VERSION [0-9]+\\.[0-9]+\\.[0-9]+")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" PACKAGE_VERSION "${_dream_project_line}")
unset(_dream_project_line)

if(NOT PACKAGE_VERSION)
    set(PACKAGE_VERSION_UNSUITABLE TRUE)
    return()
endif()

set(PACKAGE_VERSION_COMPATIBLE FALSE)
if(NOT PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
    string(REPLACE "." ";" _dream_have "${PACKAGE_VERSION}")
    list(GET _dream_have 0 _dream_have_major)
    list(GET _dream_have 1 _dream_have_minor)
    if(PACKAGE_VERSION VERSION_GREATER_EQUAL PACKAGE_FIND_VERSION AND
       _dream_have_major EQUAL PACKAGE_FIND_VERSION_MAJOR)
        if(_dream_have_major GREATER 0 OR _dream_have_minor EQUAL PACKAGE_FIND_VERSION_MINOR)
            set(PACKAGE_VERSION_COMPATIBLE TRUE)
        endif()
    endif()
    unset(_dream_have)
    unset(_dream_have_major)
    unset(_dream_have_minor)
endif()
if(PACKAGE_VERSION VERSION_EQUAL PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
endif()
