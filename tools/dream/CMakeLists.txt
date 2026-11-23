# The `dream` command (ADR 0045), which makes new game projects from templates/game. It runs on the
# build machine, so it is only built when the build is for that machine, and `cmake --install`
# puts it in the SDK's bin folder.
add_executable(dream dream.cpp)
target_link_libraries(dream PRIVATE dream::core)
dream_configure_target(dream)
# A build in the engine's checkout finds the template there, before it is installed.
target_compile_definitions(dream PRIVATE "DREAM_ENGINE_SOURCE_DIR=\"${DreamEngine_SOURCE_DIR}\"")
if(DREAM_INSTALL_SDK)
    install(TARGETS dream RUNTIME DESTINATION bin)
endif()

if(DREAM_BUILD_TESTS)
    add_test(NAME dream_new
        COMMAND ${CMAKE_COMMAND} "-DDREAM=$<TARGET_FILE:dream>"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/dream_new_test"
            -P "${DreamEngine_SOURCE_DIR}/cmake/tests/dream_new_test.cmake")
endif()
