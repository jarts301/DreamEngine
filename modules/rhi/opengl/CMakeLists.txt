# OpenGL 4.3 core, for Windows. The device makes its own context on the window it is given, through
# WGL, so the platform layer needs nothing for it. OpenGL ES is the 'opengles' module, built from
# the same source (ADR 0026).
if(NOT WIN32)
    message(FATAL_ERROR "The 'opengl' graphics backend can only be built for Windows. On Android, "
                        "list 'opengles' instead.")
endif()

# The Khronos headers, from vcpkg. opengl32.lib only has OpenGL 1.1, and the device loads the rest.
find_path(DREAM_GL_REGISTRY_INCLUDE_DIR GL/glcorearb.h REQUIRED)

dream_add_module(rhi opengl
    SOURCES src/gl_device.cpp
    LIBRARIES opengl32 gdi32 user32
    SHADER_SUFFIX .glsl
    SHADER_TARGET glsl
    SHADER_PROFILE 430
)
target_include_directories(dream_module_rhi_opengl SYSTEM PRIVATE "${DREAM_GL_REGISTRY_INCLUDE_DIR}")
target_compile_definitions(dream_module_rhi_opengl PRIVATE DREAM_GL_ES=0)

if(DREAM_BUILD_TESTS)
    # Needs a GPU driver with OpenGL 4.3. Without one, such as on a build server, the tests that
    # need a context say so and pass.
    dream_add_test(dream_rhi_opengl_tests
        SOURCES
            tests/test_main.cpp
            tests/gl_device_test.cpp
        LIBRARIES
            dream::module_rhi_opengl
            user32
    )
    target_compile_definitions(dream_rhi_opengl_tests PRIVATE DREAM_GL_ES=0)
endif()
