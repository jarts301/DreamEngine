# Checks `dream new` (ADR 0045): run with -DDREAM=<the tool> -DOUTPUT=<an empty folder to use>.
# Makes a project, checks its files and that every placeholder was filled in, checks the version
# file accepts the template's find_package version, and that bad names and IDs, and full folders,
# are refused.

if(NOT DREAM OR NOT OUTPUT)
    message(FATAL_ERROR "Pass -DDREAM=<dream tool> -DOUTPUT=<folder>")
endif()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
set(project_dir "${OUTPUT}/space_crates")

execute_process(COMMAND "${DREAM}" new space_crates --path "${project_dir}"
        --id org.example.spacecrates
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dream new failed (${result}): ${errors}")
endif()
if(NOT output MATCHES "Made 'Space Crates'")
    message(FATAL_ERROR "Unexpected output: ${output}")
endif()

foreach(file IN ITEMS CMakeLists.txt CMakePresets.json vcpkg.json dream.toml README.md .gitignore
        src/space_crates.cpp assets/scenes/main.toml assets/models/crate.glb
        assets/models/textures/crate.png)
    if(NOT EXISTS "${project_dir}/${file}")
        message(FATAL_ERROR "dream new didn't make ${file}")
    endif()
endforeach()

file(GLOB_RECURSE texts "${project_dir}/*.txt" "${project_dir}/*.json" "${project_dir}/*.toml"
    "${project_dir}/*.cpp" "${project_dir}/*.md")
foreach(text IN LISTS texts)
    file(READ "${text}" content)
    if(content MATCHES "__[A-Z_]+__")
        message(FATAL_ERROR "${text} still has a placeholder: ${CMAKE_MATCH_0}")
    endif()
endforeach()

file(READ "${project_dir}/CMakeLists.txt" lists)
if(NOT lists MATCHES "project\\(space_crates " OR
        NOT lists MATCHES "APPLICATION_ID org.example.spacecrates"
        OR NOT lists MATCHES "DISPLAY_NAME \"Space Crates\"")
    message(FATAL_ERROR "CMakeLists.txt wasn't filled in:\n${lists}")
endif()
string(REGEX MATCH "find_package\\(DreamEngine ([0-9.]+) REQUIRED\\)" found "${lists}")
set(wanted "${CMAKE_MATCH_1}")

file(READ "${project_dir}/vcpkg.json" manifest)
string(JSON name GET "${manifest}" name)
string(JSON version GET "${manifest}" version-semver)
if(NOT name STREQUAL "space-crates" OR NOT version STREQUAL "0.1.0" OR manifest MATCHES "doctest")
    message(FATAL_ERROR "vcpkg.json wasn't made from the SDK's:\n${manifest}")
endif()

# The version the template asks for is the engine's own.
get_filename_component(sdk "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PACKAGE_FIND_VERSION "${wanted}")
string(REPLACE "." ";" parts "${wanted}")
list(GET parts 0 PACKAGE_FIND_VERSION_MAJOR)
list(GET parts 1 PACKAGE_FIND_VERSION_MINOR)
include("${sdk}/cmake/DreamEngineConfigVersion.cmake")
if(NOT PACKAGE_VERSION_COMPATIBLE)
    message(FATAL_ERROR "The engine ${PACKAGE_VERSION} doesn't accept the template's ${wanted}")
endif()
set(PACKAGE_FIND_VERSION "9.0")
set(PACKAGE_FIND_VERSION_MAJOR 9)
set(PACKAGE_FIND_VERSION_MINOR 0)
include("${sdk}/cmake/DreamEngineConfigVersion.cmake")
if(PACKAGE_VERSION_COMPATIBLE)
    message(FATAL_ERROR "The engine ${PACKAGE_VERSION} accepts 9.0")
endif()

foreach(refused IN ITEMS "new;Bad-Name" "new;space_crates;--path;${project_dir}"
        "new;other;--path;${OUTPUT}/other;--id;nodots" "new" "unknown")
    execute_process(COMMAND "${DREAM}" ${refused} RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
        message(FATAL_ERROR "dream ${refused} should have failed")
    endif()
endforeach()

message(STATUS "dream new made a complete project")
