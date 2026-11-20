# Third-party libraries, kept in the repository so the engine builds with no package manager and no
# network (ADR 0039). All are header-only and under MIT or public-domain licenses; each folder holds
# its license. third_party/README.md says where each came from.
#
# The targets keep the names the libraries' own CMake packages use, so the rest of the build reads
# the same as with a package manager.

function(_dream_third_party name directory)
    add_library(${name} INTERFACE)
    # SYSTEM, so the engine's strict warnings don't apply to code it doesn't own.
    target_include_directories(${name} SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/include")
endfunction()

_dream_third_party(dream_third_party_doctest doctest)
add_library(doctest::doctest ALIAS dream_third_party_doctest)

_dream_third_party(dream_third_party_entt entt)
target_compile_features(dream_third_party_entt INTERFACE cxx_std_17)
add_library(EnTT::EnTT ALIAS dream_third_party_entt)

_dream_third_party(dream_third_party_tomlplusplus tomlplusplus)
add_library(dream::third_party_tomlplusplus ALIAS dream_third_party_tomlplusplus)

_dream_third_party(dream_third_party_cgltf cgltf)
add_library(dream::third_party_cgltf ALIAS dream_third_party_cgltf)

_dream_third_party(dream_third_party_stb stb)
add_library(dream::third_party_stb ALIAS dream_third_party_stb)

# Compiled libraries, built from their sources here with no warnings of the engine's, all for the
# CPU: they need no GPU, display or sound card.
function(_dream_third_party_compiled name directory)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "SOURCES")
    list(TRANSFORM ARG_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/")
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/include")
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${name} PRIVATE -w)
endfunction()

# MikkTSpace: tangents for normal maps (zlib license).
_dream_third_party_compiled(dream_third_party_mikktspace mikktspace SOURCES src/mikktspace.c)
add_library(mikktspace::mikktspace ALIAS dream_third_party_mikktspace)

# meshoptimizer: simpler versions of models for distant parts (MIT).
file(GLOB _meshoptimizer_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src/*.cpp")
_dream_third_party_compiled(dream_third_party_meshoptimizer meshoptimizer
    SOURCES ${_meshoptimizer_sources})
target_include_directories(dream_third_party_meshoptimizer SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src")
add_library(meshoptimizer::meshoptimizer ALIAS dream_third_party_meshoptimizer)

# Box2D: 2D physics (MIT).
file(GLOB _box2d_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/box2d"
    "${CMAKE_CURRENT_SOURCE_DIR}/box2d/src/*.c")
_dream_third_party_compiled(dream_third_party_box2d box2d SOURCES ${_box2d_sources})
set_target_properties(dream_third_party_box2d PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(dream_third_party_box2d PRIVATE -msse2)
endif()
find_package(Threads REQUIRED)
target_link_libraries(dream_third_party_box2d PUBLIC m Threads::Threads)
add_library(box2d::box2d ALIAS dream_third_party_box2d)

# Dear ImGui: the debug interface, drawn by the software renderer like any other (MIT).
_dream_third_party_compiled(dream_third_party_imgui imgui
    SOURCES src/imgui.cpp src/imgui_draw.cpp src/imgui_tables.cpp src/imgui_widgets.cpp
            src/imgui_demo.cpp)
add_library(imgui::imgui ALIAS dream_third_party_imgui)

# ozz-animation: skeletal animation, its base, runtime and offline libraries (MIT).
file(GLOB_RECURSE _ozz_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/ozz"
    "${CMAKE_CURRENT_SOURCE_DIR}/ozz/src/*.cc")
_dream_third_party_compiled(dream_third_party_ozz ozz SOURCES ${_ozz_sources})
target_include_directories(dream_third_party_ozz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ozz/src")
add_library(ozz::base ALIAS dream_third_party_ozz)
add_library(ozz::animation ALIAS dream_third_party_ozz)
add_library(ozz::animation_offline ALIAS dream_third_party_ozz)
