# OpenGL ES 3.1, for Android through EGL, and for Windows through WGL where the driver offers ES
# contexts, as NVIDIA's and AMD's do, which is how it is tested on PC. The device is the 'opengl'
# module's source, built for ES (ADR 0026).
set(gl_source_dir "${DreamEngine_SOURCE_DIR}/modules/rhi/opengl")

if(WIN32)
    find_path(DREAM_GL_REGISTRY_INCLUDE_DIR GL/glcorearb.h REQUIRED)
    set(gl_libraries opengl32 gdi32 user32)
elseif(ANDROID)
    set(gl_libraries EGL GLESv3 android)
else()
    message(FATAL_ERROR "The 'opengles' graphics backend is built for Windows and Android only.")
endif()

dream_add_module(rhi opengles
    SOURCES "${gl_source_dir}/src/gl_device.cpp"
    LIBRARIES ${gl_libraries}
    SHADER_SUFFIX .essl
    SHADER_TARGET essl
    SHADER_PROFILE 310
)
target_compile_definitions(dream_module_rhi_opengles PRIVATE DREAM_GL_ES=1)
if(WIN32)
    target_include_directories(dream_module_rhi_opengles SYSTEM PRIVATE
        "${DREAM_GL_REGISTRY_INCLUDE_DIR}")
endif()

if(DREAM_BUILD_TESTS AND WIN32)
    dream_add_test(dream_rhi_opengles_tests
        SOURCES
            "${gl_source_dir}/tests/test_main.cpp"
            "${gl_source_dir}/tests/gl_device_test.cpp"
        LIBRARIES
            dream::module_rhi_opengles
            user32
    )
    target_compile_definitions(dream_rhi_opengles_tests PRIVATE DREAM_GL_ES=1)
endif()
