# Chipmunk2D is MIT-licensed, and vcpkg installs it without a CMake package, so it is found by its
# header and library. It is double precision by default, which the library and headers agree on.
find_path(DREAM_CHIPMUNK_INCLUDE_DIR chipmunk/chipmunk.h REQUIRED)
find_library(DREAM_CHIPMUNK_LIBRARY NAMES chipmunk REQUIRED)

dream_add_module(physics2d chipmunk
    SOURCES src/chipmunk_physics.cpp
    LIBRARIES ${DREAM_CHIPMUNK_LIBRARY}
)
target_include_directories(dream_module_physics2d_chipmunk SYSTEM PRIVATE
    ${DREAM_CHIPMUNK_INCLUDE_DIR})
if(NOT MSVC)
    target_link_libraries(dream_module_physics2d_chipmunk PRIVATE m)
endif()

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_physics2d_chipmunk_tests
        SOURCES
            tests/chipmunk_physics_test.cpp
        LIBRARIES
            dream::module_physics2d_chipmunk
    )
    target_include_directories(dream_physics2d_chipmunk_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
