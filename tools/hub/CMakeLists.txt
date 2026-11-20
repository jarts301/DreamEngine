# The hub: the web page from which the engine is built, its games started and its tests run and
# read, in a browser (ADR 0039). Run it with: <build>/tools/hub/dream_hub
add_executable(dream_hub hub.cpp)
target_link_libraries(dream_hub PRIVATE dream::library_webserve)
target_compile_definitions(dream_hub PRIVATE
    DREAM_HUB_BUILD_DIR="${CMAKE_BINARY_DIR}"
    DREAM_HUB_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    DREAM_HUB_CMAKE="${CMAKE_COMMAND}"
    DREAM_HUB_CTEST="${CMAKE_CTEST_COMMAND}")
dream_configure_target(dream_hub)
