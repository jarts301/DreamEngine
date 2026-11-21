# The model cook tool (ADR 0042), which makes models' levels of detail at build time. It compiles
# the engine's own model reading, so it reads models exactly as the runtime does, and builds either
# inside the engine's build, for PC builds, or on its own, as the vcpkg port in
# cmake/ports/dream-model-cook does for builds that cross-compile, such as Android's.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(dream_model_cook LANGUAGES C CXX)
endif()

set(DREAM_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "The engine's source tree")
cmake_path(ABSOLUTE_PATH DREAM_ENGINE_ROOT NORMALIZE)

find_package(Threads REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_path(DREAM_COOK_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
find_path(DREAM_COOK_STB_INCLUDE_DIR stb_image.h REQUIRED)
find_path(DREAM_COOK_TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

set(root "${DREAM_ENGINE_ROOT}")
add_executable(dream_model_cook
    model_cook.cpp
    "${root}/runtime/src/model_parse.cpp"
    "${root}/modules/mesh_simplifier/meshoptimizer/src/meshoptimizer_simplifier.cpp"
    "${root}/modules/tangent_generator/mikktspace/src/mikktspace_tangent_generator.cpp"
    "${root}/modules/tangent_generator/null/src/null_tangent_generator.cpp"
    "${root}/core/src/archive.cpp"
    "${root}/core/src/assert.cpp"
    "${root}/core/src/config.cpp"
    "${root}/core/src/crash_handler_linux.cpp"
    "${root}/core/src/crash_handler_windows.cpp"
    "${root}/core/src/crash_report.cpp"
    "${root}/core/src/job_system.cpp"
    "${root}/core/src/linear_allocator.cpp"
    "${root}/core/src/log.cpp"
    "${root}/core/src/memory.cpp"
    "${root}/core/src/memory_reserve.cpp"
    "${root}/core/src/module_project.cpp"
    "${root}/core/src/module_registry.cpp"
    "${root}/core/src/pool_allocator.cpp"
    "${root}/core/src/process_stats.cpp"
    "${root}/core/src/profile.cpp"
    "${root}/core/src/version.cpp"
)
target_compile_features(dream_model_cook PRIVATE cxx_std_20)
file(GLOB interface_includes LIST_DIRECTORIES true "${root}/interfaces/*/include")
target_include_directories(dream_model_cook PRIVATE
    "${root}/core/include" "${root}/runtime/include" ${interface_includes})
target_include_directories(dream_model_cook SYSTEM PRIVATE
    ${DREAM_COOK_CGLTF_INCLUDE_DIR} ${DREAM_COOK_STB_INCLUDE_DIR}
    ${DREAM_COOK_TOMLPLUSPLUS_INCLUDE_DIR})
target_link_libraries(dream_model_cook PRIVATE
    Threads::Threads meshoptimizer::meshoptimizer mikktspace::mikktspace)
target_compile_definitions(dream_model_cook PRIVATE
    DREAM_VERSION_MAJOR=0 DREAM_VERSION_MINOR=0 DREAM_VERSION_PATCH=0)
if(WIN32)
    target_link_libraries(dream_model_cook PRIVATE psapi dbghelp)
endif()
# The engine's settings: inside its build, its own function; on its own, the same essentials.
if(COMMAND dream_configure_target)
    dream_configure_target(dream_model_cook)
elseif(MSVC)
    target_compile_options(dream_model_cook PRIVATE
        /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHs-c- /GR-)
    target_compile_definitions(dream_model_cook PRIVATE _HAS_EXCEPTIONS=0 NOMINMAX
        WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(dream_model_cook PRIVATE -fno-exceptions -fno-rtti)
endif()

install(TARGETS dream_model_cook RUNTIME DESTINATION bin)
