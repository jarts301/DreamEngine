// The OpenGL backends: 'opengl', OpenGL 4.3 core on Windows, and 'opengles', OpenGL ES 3.1 on
// Android and on Windows drivers that offer ES contexts. Both are this file, built with DREAM_GL_ES
// set to 0 or 1 (ADR 0026). The two versions share what the device uses: uniform blocks and
// textures with explicit bindings, separate vertex formats, immutable texture storage and samplers.
//
// The device makes its own context, through WGL on Windows and EGL on Android, with an offscreen
// drawable of its own for the time no window is attached, so its resources outlive the surface as
// the interface requires.

#include "dream/core/assert.h"
#include "dream/core/config.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#if defined(_WIN32)
#include <windows.h>
// Declarations only, which give each function's type. Past OpenGL 1.1, nothing is linked: the
// device loads every function through the driver.
#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>
#include <GL/wglext.h>
#define DREAM_GL_APIENTRY APIENTRY
#elif defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/native_window.h>
#define DREAM_GL_APIENTRY GL_APIENTRY
#else
#error "The OpenGL backends are built for Windows and Android only."
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if !defined(DREAM_GL_ES)
#error "Build with DREAM_GL_ES set to 0 for OpenGL or 1 for OpenGL ES."
#endif

namespace dream::modules {
namespace {

using namespace rhi;

constexpr bool k_es = DREAM_GL_ES != 0;
constexpr std::string_view k_name = k_es ? "opengles" : "opengl";
constexpr std::string_view k_api = k_es ? "OpenGL ES" : "OpenGL";
constexpr int k_major_version = k_es ? 3 : 4;
constexpr int k_minor_version = k_es ? 1 : 3;
// The first line of every shader this device takes, as DreamCrossCompileShader.cmake writes it.
constexpr std::string_view k_shader_version = k_es ? "#version 310 es" : "#version 430";

// Where every shader finds its inputs (ADR 0013, 0026, 0027): the uniform block of set_uniforms at
// block binding 0, the textures of slots 0 to 2 at bindings 1 to 3, which are texture units in
// OpenGL, and the push constants, which OpenGL has no special kind of buffer for, at block
// binding 2.
constexpr GLuint k_uniform_binding = 0;
constexpr GLuint k_texture_unit = 1; // Slot 0's; slot n's is texture_binding(n) (ADR 0027).
constexpr GLuint k_push_constant_binding = 2;
// The block of set_draw_uniforms, at the same binding as in Vulkan (ADR 0036).
constexpr GLuint k_draw_block_binding = k_draw_uniform_binding;

// Each set_uniforms and push_constants call takes a fresh slot in this buffer, which is renewed
// every frame, so a draw keeps the values it was given. When a frame uses it all, it is renewed
// again; the driver keeps what earlier draws still read.
constexpr GLsizeiptr k_uniform_ring_bytes = 4 * 1024 * 1024;

#if defined(NDEBUG)
constexpr bool k_debug_by_default = false;
#else
constexpr bool k_debug_by_default = true;
#endif

// Values from extensions, which the two sets of headers don't both define.
constexpr GLenum k_time_elapsed = 0x88BF;            // GL_TIME_ELAPSED(_EXT)
constexpr GLenum k_gpu_disjoint = 0x8FBB;            // GL_GPU_DISJOINT_EXT
constexpr GLenum k_bc7_unorm = 0x8E8C;               // GL_COMPRESSED_RGBA_BPTC_UNORM
constexpr GLenum k_bc7_srgb = 0x8E8D;                // GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
constexpr GLenum k_astc_4x4_unorm = 0x93B0;          // GL_COMPRESSED_RGBA_ASTC_4x4_KHR
constexpr GLenum k_astc_4x4_srgb = 0x93D0;           // GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR
#if defined(_WIN32)
constexpr GLenum k_framebuffer_srgb = 0x8DB9; // GL_FRAMEBUFFER_SRGB(_EXT)
#endif
constexpr GLenum k_debug_output = 0x92E0;            // GL_DEBUG_OUTPUT
constexpr GLenum k_debug_output_synchronous = 0x8242; // GL_DEBUG_OUTPUT_SYNCHRONOUS
constexpr GLenum k_debug_severity_high = 0x9146;
constexpr GLenum k_debug_severity_medium = 0x9147;
constexpr GLenum k_debug_type_performance = 0x8250;
// The uniform DreamCrossCompileShader.cmake adds to every vertex shader, by which the device turns
// the picture upside down when it draws into a texture: OpenGL stores a texture's rows from the
// bottom, and the other APIs from the top.
constexpr const char* k_clip_flip_uniform = "dream_clip_flip";

// What the context's loader gives, cast to each function's own type.
using Proc = void (*)();

core::Error invalid(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

core::Error failure(std::string message) {
    return core::Error{core::ErrorCode::Unknown, std::move(message)};
}

// Every function the device calls, as members of Functions. The two lists are what OpenGL 4.3 and
// OpenGL ES 3.1 have in common.
#define DREAM_GL_FUNCTIONS(F)                                                                      \
    F(ActiveTexture, glActiveTexture)                                                              \
    F(AttachShader, glAttachShader)                                                                \
    F(BeginQuery, glBeginQuery)                                                                    \
    F(BindBuffer, glBindBuffer)                                                                    \
    F(BindBufferRange, glBindBufferRange)                                                          \
    F(BindFramebuffer, glBindFramebuffer)                                                          \
    F(BindRenderbuffer, glBindRenderbuffer)                                                        \
    F(BindSampler, glBindSampler)                                                                  \
    F(BindTexture, glBindTexture)                                                                  \
    F(BindVertexArray, glBindVertexArray)                                                          \
    F(BindVertexBuffer, glBindVertexBuffer)                                                        \
    F(BlendFuncSeparate, glBlendFuncSeparate)                                                      \
    F(BlendEquation, glBlendEquation)                                                              \
    F(BufferData, glBufferData)                                                                    \
    F(BufferSubData, glBufferSubData)                                                              \
    F(CheckFramebufferStatus, glCheckFramebufferStatus)                                            \
    F(Clear, glClear)                                                                              \
    F(ClearColor, glClearColor)                                                                    \
    F(ClearDepthf, glClearDepthf)                                                                  \
    F(ColorMask, glColorMask)                                                                      \
    F(CompileShader, glCompileShader)                                                              \
    F(CompressedTexSubImage2D, glCompressedTexSubImage2D)                                          \
    F(CreateProgram, glCreateProgram)                                                              \
    F(CreateShader, glCreateShader)                                                                \
    F(CullFace, glCullFace)                                                                        \
    F(DeleteBuffers, glDeleteBuffers)                                                              \
    F(DeleteFramebuffers, glDeleteFramebuffers)                                                    \
    F(DeleteProgram, glDeleteProgram)                                                              \
    F(DeleteQueries, glDeleteQueries)                                                              \
    F(DeleteRenderbuffers, glDeleteRenderbuffers)                                                  \
    F(DeleteSamplers, glDeleteSamplers)                                                            \
    F(DeleteShader, glDeleteShader)                                                                \
    F(DeleteTextures, glDeleteTextures)                                                            \
    F(DeleteVertexArrays, glDeleteVertexArrays)                                                    \
    F(DepthFunc, glDepthFunc)                                                                      \
    F(DepthMask, glDepthMask)                                                                      \
    F(DetachShader, glDetachShader)                                                                \
    F(Disable, glDisable)                                                                          \
    F(DisableVertexAttribArray, glDisableVertexAttribArray)                                        \
    F(DrawArrays, glDrawArrays)                                                                    \
    F(DrawElements, glDrawElements)                                                                \
    F(DrawElementsInstanced, glDrawElementsInstanced)                                              \
    F(Enable, glEnable)                                                                            \
    F(EnableVertexAttribArray, glEnableVertexAttribArray)                                          \
    F(EndQuery, glEndQuery)                                                                        \
    F(FramebufferRenderbuffer, glFramebufferRenderbuffer)                                          \
    F(FramebufferTexture2D, glFramebufferTexture2D)                                                \
    F(Finish, glFinish)                                                                            \
    F(FrontFace, glFrontFace)                                                                      \
    F(GenBuffers, glGenBuffers)                                                                    \
    F(GenFramebuffers, glGenFramebuffers)                                                          \
    F(GenQueries, glGenQueries)                                                                    \
    F(GenRenderbuffers, glGenRenderbuffers)                                                        \
    F(GenSamplers, glGenSamplers)                                                                  \
    F(GenTextures, glGenTextures)                                                                  \
    F(GenVertexArrays, glGenVertexArrays)                                                          \
    F(GenerateMipmap, glGenerateMipmap)                                                            \
    F(GetActiveAttrib, glGetActiveAttrib)                                                          \
    F(GetActiveUniform, glGetActiveUniform)                                                        \
    F(GetActiveUniformBlockName, glGetActiveUniformBlockName)                                      \
    F(GetActiveUniformBlockiv, glGetActiveUniformBlockiv)                                          \
    F(GetAttribLocation, glGetAttribLocation)                                                      \
    F(GetError, glGetError)                                                                        \
    F(GetIntegerv, glGetIntegerv)                                                                  \
    F(GetProgramInfoLog, glGetProgramInfoLog)                                                      \
    F(GetProgramiv, glGetProgramiv)                                                                \
    F(GetQueryObjectuiv, glGetQueryObjectuiv)                                                      \
    F(GetShaderInfoLog, glGetShaderInfoLog)                                                        \
    F(GetShaderiv, glGetShaderiv)                                                                  \
    F(GetString, glGetString)                                                                      \
    F(GetStringi, glGetStringi)                                                                    \
    F(GetUniformLocation, glGetUniformLocation)                                                    \
    F(GetUniformiv, glGetUniformiv)                                                                \
    F(LinkProgram, glLinkProgram)                                                                  \
    F(PixelStorei, glPixelStorei)                                                                  \
    F(PolygonOffset, glPolygonOffset)                                                              \
    F(ReadPixels, glReadPixels)                                                                    \
    F(RenderbufferStorage, glRenderbufferStorage)                                                  \
    F(SamplerParameteri, glSamplerParameteri)                                                      \
    F(Scissor, glScissor)                                                                          \
    F(ShaderSource, glShaderSource)                                                                \
    F(TexParameteri, glTexParameteri)                                                              \
    F(TexStorage2D, glTexStorage2D)                                                                \
    F(TexSubImage2D, glTexSubImage2D)                                                              \
    F(Uniform1f, glUniform1f)                                                                      \
    F(UseProgram, glUseProgram)                                                                    \
    F(VertexAttribBinding, glVertexAttribBinding)                                                  \
    F(VertexAttribFormat, glVertexAttribFormat)                                                    \
    F(VertexBindingDivisor, glVertexBindingDivisor)                                                \
    F(Viewport, glViewport)

struct Functions {
#define DREAM_GL_MEMBER(member, symbol) decltype(&::symbol) member = nullptr;
    DREAM_GL_FUNCTIONS(DREAM_GL_MEMBER)
#undef DREAM_GL_MEMBER
    // Optional: from KHR_debug, core in OpenGL 4.3 and OpenGL ES 3.2.
    decltype(&::glDebugMessageCallback) DebugMessageCallback = nullptr;
};

// Errors and misuse. Performance notes, such as that reading a frame back waits for the GPU, are
// expected where they happen and are left out.
void DREAM_GL_APIENTRY on_debug_message(GLenum /*source*/, GLenum type, GLuint /*id*/,
                                        GLenum severity, GLsizei length, const GLchar* message,
                                        const void* /*user*/) {
    const std::string_view text =
        length >= 0 ? std::string_view(message, static_cast<std::size_t>(length))
                    : std::string_view(message);
    if (type == k_debug_type_performance) {
        return;
    }
    if (severity == k_debug_severity_high) {
        DREAM_LOG_ERROR("{}: {}", k_api, text);
    } else if (severity == k_debug_severity_medium) {
        DREAM_LOG_WARNING("{}: {}", k_api, text);
    }
}

// ---------------------------------------------------------------------------------------------
// The context, and the window or offscreen drawable it draws to.

#if defined(_WIN32)

constexpr wchar_t k_helper_class[] = L"DreamOpenGLHelper";

HWND create_helper_window() {
    WNDCLASSW window_class{};
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = &DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = k_helper_class;
    RegisterClassW(&window_class); // Fails harmlessly once the class exists.
    return CreateWindowExW(0, k_helper_class, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                           nullptr, window_class.hInstance, nullptr);
}

std::string windows_error(std::string_view action) {
    return std::format("{} failed with Windows error {}", action, GetLastError());
}

// A WGL context. Choosing a modern pixel format and context needs WGL extensions, which only a
// current context can load, so a first, plain one is made on a throwaway window to find them.
class Context {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context() {
        if (m_context != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(m_context);
        }
        if (m_helper_dc != nullptr) {
            ReleaseDC(m_helper, m_helper_dc);
        }
        if (m_helper != nullptr) {
            DestroyWindow(m_helper);
        }
        m_opengl32 = nullptr;
    }

    core::Result<void> create(bool debug) {
        if (auto loaded = load_extensions(); !loaded.has_value()) {
            return loaded;
        }

        m_helper = create_helper_window();
        if (m_helper == nullptr) {
            return failure(windows_error("Creating OpenGL's helper window"));
        }
        m_helper_dc = GetDC(m_helper);

        const std::array<int, 23> attributes{
            WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
            WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
            WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
            WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_RED_BITS_ARB, 8,
            WGL_GREEN_BITS_ARB, 8,
            WGL_BLUE_BITS_ARB, 8,
            WGL_ALPHA_BITS_ARB, 8,
            WGL_DEPTH_BITS_ARB, 24,
            // The window is drawn with sRGB encoding, as the other backends' windows are.
            WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, GL_TRUE,
            0};
        UINT count = 0;
        if (m_choose_pixel_format(m_helper_dc, attributes.data(), nullptr, 1, &m_pixel_format,
                                  &count) == FALSE ||
            count == 0) {
            return failure("No pixel format for OpenGL has 8-bit sRGB color and 24-bit depth");
        }
        DescribePixelFormat(m_helper_dc, m_pixel_format, sizeof(m_descriptor), &m_descriptor);
        if (SetPixelFormat(m_helper_dc, m_pixel_format, &m_descriptor) == FALSE) {
            return failure(windows_error("Setting the helper window's pixel format"));
        }

        const int profile = k_es ? WGL_CONTEXT_ES2_PROFILE_BIT_EXT : WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
        if constexpr (k_es) {
            if (!m_has_es_contexts) {
                return failure("This OpenGL driver can't make OpenGL ES contexts "
                               "(WGL_EXT_create_context_es2_profile)");
            }
        }
        for (const bool with_debug : {debug, false}) {
            const std::array<int, 9> context_attributes{
                WGL_CONTEXT_MAJOR_VERSION_ARB, k_major_version,
                WGL_CONTEXT_MINOR_VERSION_ARB, k_minor_version,
                WGL_CONTEXT_PROFILE_MASK_ARB, profile,
                WGL_CONTEXT_FLAGS_ARB, with_debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
                0};
            m_context = m_create_context(m_helper_dc, nullptr, context_attributes.data());
            if (m_context != nullptr) {
                break;
            }
        }
        if (m_context == nullptr) {
            return failure(std::format("The driver can't make an {} {}.{} context", k_api,
                                       k_major_version, k_minor_version));
        }
        if (wglMakeCurrent(m_helper_dc, m_context) == FALSE) {
            return failure(windows_error("Making the OpenGL context current"));
        }
        m_opengl32 = GetModuleHandleW(L"opengl32.dll");
        return {};
    }

    core::Result<void> attach(const platform::NativeWindow& window) {
        const auto* const win32 = std::get_if<platform::Win32Window>(&window);
        if (win32 == nullptr || win32->window == nullptr) {
            return invalid(std::format("{} needs a Windows window here", k_api));
        }
        const auto hwnd = static_cast<HWND>(win32->window);
        HDC dc = GetDC(hwnd);
        if (dc == nullptr) {
            return failure(windows_error("Getting the window's device context"));
        }
        // A window's pixel format can be set only once, and the context draws only to windows
        // with its own.
        const int current = GetPixelFormat(dc);
        if (current == 0) {
            if (SetPixelFormat(dc, m_pixel_format, &m_descriptor) == FALSE) {
                ReleaseDC(hwnd, dc);
                return failure(windows_error("Setting the window's pixel format"));
            }
        } else if (current != m_pixel_format) {
            ReleaseDC(hwnd, dc);
            return failure("The window already has another pixel format, so OpenGL can't draw "
                           "to it");
        }
        if (wglMakeCurrent(dc, m_context) == FALSE) {
            ReleaseDC(hwnd, dc);
            return failure(windows_error("Making the OpenGL context current on the window"));
        }
        m_window = hwnd;
        m_window_dc = dc;
        // Paced to the display, like the other backends.
        if (m_swap_interval != nullptr) {
            m_swap_interval(1);
        }
        return {};
    }

    void detach() {
        if (m_window == nullptr) {
            return;
        }
        wglMakeCurrent(m_helper_dc, m_context);
        ReleaseDC(m_window, m_window_dc);
        m_window = nullptr;
        m_window_dc = nullptr;
    }

    [[nodiscard]] bool has_window() const {
        return m_window != nullptr;
    }

    [[nodiscard]] platform::SurfaceSize window_size() const {
        RECT rect{};
        if (m_window == nullptr || GetClientRect(m_window, &rect) == FALSE) {
            return {};
        }
        return platform::SurfaceSize{static_cast<std::uint32_t>(rect.right - rect.left),
                                     static_cast<std::uint32_t>(rect.bottom - rect.top)};
    }

    core::Result<void> present() {
        if (SwapBuffers(m_window_dc) == FALSE) {
            return failure(windows_error("Presenting an OpenGL frame"));
        }
        return {};
    }

    // wglGetProcAddress gives only what came after OpenGL 1.1, and some drivers return small
    // numbers rather than null for what they lack.
    [[nodiscard]] Proc function(const char* name) const {
        PROC address = wglGetProcAddress(name);
        const auto value = reinterpret_cast<std::intptr_t>(address);
        if (value >= -1 && value <= 3) {
            address = m_opengl32 != nullptr ? GetProcAddress(m_opengl32, name) : nullptr;
        }
        return reinterpret_cast<Proc>(address);
    }

private:
    core::Result<void> load_extensions() {
        HWND window = create_helper_window();
        if (window == nullptr) {
            return failure(windows_error("Creating a window to start OpenGL"));
        }
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cDepthBits = 24;
        const int format = ChoosePixelFormat(dc, &descriptor);
        HGLRC context = nullptr;
        if (format != 0 && SetPixelFormat(dc, format, &descriptor) != FALSE) {
            context = wglCreateContext(dc);
        }
        if (context != nullptr && wglMakeCurrent(dc, context) != FALSE) {
            m_choose_pixel_format = reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(
                wglGetProcAddress("wglChoosePixelFormatARB"));
            m_create_context = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
                wglGetProcAddress("wglCreateContextAttribsARB"));
            m_swap_interval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
                wglGetProcAddress("wglSwapIntervalEXT"));
            const auto get_extensions = reinterpret_cast<PFNWGLGETEXTENSIONSSTRINGARBPROC>(
                wglGetProcAddress("wglGetExtensionsStringARB"));
            if (get_extensions != nullptr) {
                const char* const list = get_extensions(dc);
                const std::string_view extensions = list != nullptr ? list : "";
                m_has_es_contexts =
                    extensions.find("WGL_EXT_create_context_es2_profile") != std::string_view::npos;
            }
            wglMakeCurrent(nullptr, nullptr);
        }
        if (context != nullptr) {
            wglDeleteContext(context);
        }
        ReleaseDC(window, dc);
        DestroyWindow(window);

        if (m_choose_pixel_format == nullptr || m_create_context == nullptr) {
            return failure("No OpenGL driver with WGL_ARB_create_context and "
                           "WGL_ARB_pixel_format is installed");
        }
        return {};
    }

    PFNWGLCHOOSEPIXELFORMATARBPROC m_choose_pixel_format = nullptr;
    PFNWGLCREATECONTEXTATTRIBSARBPROC m_create_context = nullptr;
    PFNWGLSWAPINTERVALEXTPROC m_swap_interval = nullptr;
    bool m_has_es_contexts = false;
    HMODULE m_opengl32 = nullptr;
    HWND m_helper = nullptr;
    HDC m_helper_dc = nullptr;
    int m_pixel_format = 0;
    PIXELFORMATDESCRIPTOR m_descriptor{};
    HGLRC m_context = nullptr;
    HWND m_window = nullptr;
    HDC m_window_dc = nullptr;
};

#elif defined(__ANDROID__)

std::string egl_error(std::string_view action) {
    return std::format("{} failed with EGL error 0x{:04X}", action,
                       static_cast<unsigned>(eglGetError()));
}

// An EGL context, with a 1x1 pbuffer to be current on while Android has taken the window away.
class Context {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context() {
        if (m_display == EGL_NO_DISPLAY) {
            return;
        }
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_window_surface != EGL_NO_SURFACE) {
            eglDestroySurface(m_display, m_window_surface);
        }
        if (m_pbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(m_display, m_pbuffer);
        }
        if (m_context != EGL_NO_CONTEXT) {
            eglDestroyContext(m_display, m_context);
        }
        eglTerminate(m_display);
    }

    core::Result<void> create(bool debug) {
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY || eglInitialize(m_display, nullptr, nullptr) == EGL_FALSE) {
            m_display = EGL_NO_DISPLAY;
            return failure(egl_error("Starting EGL"));
        }
        const char* const extension_list = eglQueryString(m_display, EGL_EXTENSIONS);
        const std::string_view extensions = extension_list != nullptr ? extension_list : "";
        m_has_srgb_surfaces = extensions.find("EGL_KHR_gl_colorspace") != std::string_view::npos;
        eglBindAPI(EGL_OPENGL_ES_API);

        constexpr EGLint k_es3_bit = 0x0040; // EGL_OPENGL_ES3_BIT
        const std::array<EGLint, 17> attributes{
            EGL_RENDERABLE_TYPE, k_es3_bit,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE};
        EGLint count = 0;
        if (eglChooseConfig(m_display, attributes.data(), &m_config, 1, &count) == EGL_FALSE ||
            count == 0) {
            return failure("No EGL configuration has OpenGL ES 3, 8-bit color and 24-bit depth");
        }

        constexpr EGLint k_minor_version_attribute = 0x30FB; // EGL_CONTEXT_MINOR_VERSION_KHR
        constexpr EGLint k_flags_attribute = 0x30FC;         // EGL_CONTEXT_FLAGS_KHR
        constexpr EGLint k_debug_bit = 0x0001;               // EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR
        for (const bool with_debug : {debug, false}) {
            const std::array<EGLint, 7> context_attributes{
                EGL_CONTEXT_CLIENT_VERSION, k_major_version,
                k_minor_version_attribute, k_minor_version,
                k_flags_attribute, with_debug ? k_debug_bit : 0,
                EGL_NONE};
            m_context =
                eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, context_attributes.data());
            if (m_context != EGL_NO_CONTEXT) {
                break;
            }
        }
        if (m_context == EGL_NO_CONTEXT) {
            return failure(egl_error("Creating an OpenGL ES 3.1 context"));
        }

        const std::array<EGLint, 5> pbuffer_attributes{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m_pbuffer = eglCreatePbufferSurface(m_display, m_config, pbuffer_attributes.data());
        if (m_pbuffer == EGL_NO_SURFACE) {
            return failure(egl_error("Creating EGL's offscreen surface"));
        }
        if (eglMakeCurrent(m_display, m_pbuffer, m_pbuffer, m_context) == EGL_FALSE) {
            return failure(egl_error("Making the OpenGL ES context current"));
        }
        return {};
    }

    core::Result<void> attach(const platform::NativeWindow& window) {
        const auto* const android = std::get_if<platform::AndroidWindow>(&window);
        if (android == nullptr || android->window == nullptr) {
            return invalid("OpenGL ES needs an Android window here");
        }
        auto* const native = static_cast<ANativeWindow*>(android->window);
        EGLint format = 0;
        eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(native, 0, 0, format);

        // The window is drawn with sRGB encoding, as the other backends' windows are.
        constexpr EGLint k_colorspace = 0x309D;      // EGL_GL_COLORSPACE_KHR
        constexpr EGLint k_colorspace_srgb = 0x3089; // EGL_GL_COLORSPACE_SRGB_KHR
        std::array<EGLint, 3> attributes{EGL_NONE, EGL_NONE, EGL_NONE};
        if (m_has_srgb_surfaces) {
            attributes = {k_colorspace, k_colorspace_srgb, EGL_NONE};
        } else {
            DREAM_LOG_WARNING("EGL can't make sRGB windows (EGL_KHR_gl_colorspace), so colors "
                              "will look too dark");
        }
        m_window_surface = eglCreateWindowSurface(m_display, m_config, native, attributes.data());
        if (m_window_surface == EGL_NO_SURFACE) {
            return failure(egl_error("Creating the window's EGL surface"));
        }
        if (eglMakeCurrent(m_display, m_window_surface, m_window_surface, m_context) ==
            EGL_FALSE) {
            const core::Error error = failure(egl_error("Making the context current on the window"));
            eglDestroySurface(m_display, m_window_surface);
            m_window_surface = EGL_NO_SURFACE;
            return error;
        }
        eglSwapInterval(m_display, 1);
        return {};
    }

    void detach() {
        if (m_window_surface == EGL_NO_SURFACE) {
            return;
        }
        eglMakeCurrent(m_display, m_pbuffer, m_pbuffer, m_context);
        eglDestroySurface(m_display, m_window_surface);
        m_window_surface = EGL_NO_SURFACE;
    }

    [[nodiscard]] bool has_window() const {
        return m_window_surface != EGL_NO_SURFACE;
    }

    [[nodiscard]] platform::SurfaceSize window_size() const {
        EGLint width = 0;
        EGLint height = 0;
        if (m_window_surface == EGL_NO_SURFACE ||
            eglQuerySurface(m_display, m_window_surface, EGL_WIDTH, &width) == EGL_FALSE ||
            eglQuerySurface(m_display, m_window_surface, EGL_HEIGHT, &height) == EGL_FALSE) {
            return {};
        }
        return platform::SurfaceSize{static_cast<std::uint32_t>(std::max(width, 0)),
                                     static_cast<std::uint32_t>(std::max(height, 0))};
    }

    core::Result<void> present() {
        if (eglSwapBuffers(m_display, m_window_surface) == EGL_FALSE) {
            const EGLint error = eglGetError();
            if (error == EGL_CONTEXT_LOST) {
                return failure("Presenting an OpenGL ES frame: the context was lost");
            }
            // The surface going away is Android taking the window, which detach_surface follows.
            if (error != EGL_BAD_SURFACE && error != EGL_BAD_NATIVE_WINDOW) {
                return failure(std::format("Presenting an OpenGL ES frame failed with EGL "
                                           "error 0x{:04X}",
                                           static_cast<unsigned>(error)));
            }
        }
        return {};
    }

    [[nodiscard]] Proc function(const char* name) const {
        return reinterpret_cast<Proc>(eglGetProcAddress(name));
    }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_pbuffer = EGL_NO_SURFACE;
    EGLSurface m_window_surface = EGL_NO_SURFACE;
    bool m_has_srgb_surfaces = false;
};

#endif

// ---------------------------------------------------------------------------------------------
// The device.

struct BufferResource {
    GLuint id = 0;
};

struct ShaderResource {
    GLuint id = 0;
    ShaderStage stage = ShaderStage::Vertex;
};

struct TextureResource {
    GLuint id = 0;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    platform::SurfaceSize size;
    bool render_target = false;
    // Its rows can be replaced with update_texture (ADR 0037).
    bool dynamic = false;
};

// A framebuffer object for one pair of render targets, made the first time a pass uses them.
struct TargetFramebuffer {
    GLuint color = 0;
    GLuint depth = 0;
    GLuint framebuffer = 0;
};

// The program holds compiled copies of its shaders, so a pipeline keeps working after its shaders
// are destroyed, as it does on Vulkan.
struct PipelineResource {
    GLuint program = 0;
    GLenum mode = GL_TRIANGLES;
    GLsizei vertex_stride = 0;
    std::vector<VertexAttribute> attributes;
    // Read from vertex buffer binding 1, once per instance.
    GLsizei instance_stride = 0;
    std::vector<VertexAttribute> instance_attributes;
    BlendMode blend = BlendMode::Opaque;
    bool depth_test = true;
    bool depth_write = true;
    CullMode cull = CullMode::Back;
    float depth_bias = 0.0f;
    float slope_depth_bias = 0.0f;
    std::optional<TextureFormat> color_target;
    std::optional<TextureFormat> depth_target;
    GLint clip_flip = -1;
};

struct TimingQuery {
    GLuint id = 0;
};

constexpr std::size_t k_kept_gpu_frame_times = 16;
constexpr GLuint k_max_attributes = 16;

struct VertexFormatInfo {
    GLint size = 0;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
};

VertexFormatInfo to_gl(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float2:
        return {2, GL_FLOAT, GL_FALSE};
    case VertexFormat::Float3:
        return {3, GL_FLOAT, GL_FALSE};
    case VertexFormat::Float4:
        return {4, GL_FLOAT, GL_FALSE};
    case VertexFormat::UByte4Norm:
        return {4, GL_UNSIGNED_BYTE, GL_TRUE};
    }
    return {};
}

GLenum internal_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8Srgb:
        return GL_SRGB8_ALPHA8;
    case TextureFormat::Rgba8Unorm:
        return GL_RGBA8;
    case TextureFormat::Bc7Srgb:
        return k_bc7_srgb;
    case TextureFormat::Bc7Unorm:
        return k_bc7_unorm;
    case TextureFormat::Astc4x4Srgb:
        return k_astc_4x4_srgb;
    case TextureFormat::Astc4x4Unorm:
        return k_astc_4x4_unorm;
    case TextureFormat::Rgba16Float:
        return GL_RGBA16F;
    case TextureFormat::Rg11B10Float:
        return GL_R11F_G11F_B10F;
    case TextureFormat::Depth32Float:
        return GL_DEPTH_COMPONENT32F;
    case TextureFormat::Rgba32Float:
        return GL_RGBA32F;
    }
    return GL_RGBA8;
}

// The type of each component of an uncompressed texture's pixels.
GLenum pixel_type(TextureFormat format) {
    if (format == TextureFormat::Rgba32Float) {
        return GL_FLOAT;
    }
    return format == TextureFormat::Rgba16Float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
}

class GlDevice final : public Device, private CommandList {
public:
    GlDevice() = default;
    GlDevice(const GlDevice&) = delete;
    GlDevice& operator=(const GlDevice&) = delete;

    ~GlDevice() override {
        warn_about_leftovers();
        if (!m_ready) {
            return;
        }
        // OpenGL doesn't keep objects alive for their users, so whatever is left is deleted here,
        // while the context still exists.
        m_textures.for_each([&](auto, TextureResource& texture) { delete_texture(texture); });
        m_buffers.for_each([&](auto, BufferResource& buffer) { m_gl.DeleteBuffers(1, &buffer.id); });
        m_shaders.for_each([&](auto, ShaderResource& shader) { m_gl.DeleteShader(shader.id); });
        m_pipelines.for_each(
            [&](auto, PipelineResource& pipeline) { m_gl.DeleteProgram(pipeline.program); });
        delete_texture(m_white_texture);
        delete_texture(m_far_depth_texture);
        for (const TargetFramebuffer& target : m_target_framebuffers) {
            m_gl.DeleteFramebuffers(1, &target.framebuffer);
        }
        if (m_empty_fragment_shader != 0) {
            m_gl.DeleteShader(m_empty_fragment_shader);
        }
        delete_offscreen_target();
        if (m_copy_program != 0) {
            m_gl.DeleteProgram(m_copy_program);
        }
        m_gl.DeleteBuffers(1, &m_uniform_ring);
        m_gl.DeleteBuffers(1, &m_frame_buffer);
        m_gl.DeleteSamplers(static_cast<GLsizei>(m_samplers.size()), m_samplers.data());
        m_gl.DeleteVertexArrays(1, &m_vertex_array);
        for (TimingQuery& query : m_timing_queries) {
            if (query.id != 0) {
                m_gl.DeleteQueries(1, &query.id);
            }
        }
        m_gl.Finish();
        m_context.detach();
    }

    [[nodiscard]] core::Result<void> initialize(const core::Config& config) {
        const bool debug =
            config.get_bool(std::format("rhi.{}.debug", k_name)).value_or(k_debug_by_default);
        if (auto created = m_context.create(debug); !created.has_value()) {
            return created;
        }
        if (auto loaded = load_functions(); !loaded.has_value()) {
            return loaded;
        }
        m_ready = true;
        if (auto checked = check_version(); !checked.has_value()) {
            return checked;
        }
        read_extensions();
        start_debug_output(debug);
        create_shared_state();

        constexpr std::array k_white_pixel{std::byte{255}, std::byte{255}, std::byte{255},
                                           std::byte{255}};
        core::Result<TextureResource> white = upload_texture(
            TextureDesc{.width = 1, .height = 1, .generate_mipmaps = false}, k_white_pixel);
        if (!white.has_value()) {
            return white.error();
        }
        m_white_texture = white.value();

        // The shadow slot's default: a depth at the far plane, so every comparison passes.
        core::Result<TextureResource> far_depth = upload_texture(
            TextureDesc{.width = 1, .height = 1, .format = TextureFormat::Depth32Float,
                        .render_target = true},
            {});
        if (!far_depth.has_value()) {
            return far_depth.error();
        }
        m_far_depth_texture = far_depth.value();
        constexpr float k_far = 1.0f;
        bind_on_unit(0, m_far_depth_texture.id);
        m_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &k_far);
        bind_on_unit(0, m_bound_textures[0]);
        return {};
    }

    core::Result<void> attach_surface(const platform::NativeWindow& window,
                                      platform::SurfaceSize size) override {
        detach_surface();
        if (auto attached = m_context.attach(window); !attached.has_value()) {
            return attached;
        }
        m_was_attached = true;
        m_requested_size = size;
#if defined(_WIN32)
        // The pixel format is sRGB-capable, and OpenGL writes sRGB to it once this is on. Drivers'
        // ES contexts on Windows take the same switch, even where they don't list
        // EXT_sRGB_write_control. On Android, EGL's sRGB window needs none.
        m_gl.Enable(k_framebuffer_srgb);
        static_cast<void>(m_gl.GetError());
#endif
        // Whether the window really encodes sRGB is easiest to learn by trying, since drivers
        // answer the query for it unreliably. Every window this device attaches to has the same
        // configuration, so once is enough.
        if (!m_probed_window) {
            m_probed_window = true;
            m_window_encodes_srgb = window_encodes_srgb();
            if (!m_window_encodes_srgb) {
                DREAM_LOG_INFO("The {} window doesn't encode sRGB, so frames are drawn offscreen "
                               "and encoded when copied to it",
                               k_api);
            }
        }
        return {};
    }

    void detach_surface() override {
        if (!m_context.has_window()) {
            return;
        }
        m_gl.Finish();
        m_context.detach();
    }

    void resize_surface(platform::SurfaceSize size) override {
        m_requested_size = size;
    }

    std::string_view shader_file_suffix() const override {
        return k_es ? ".essl" : ".glsl";
    }

    bool supports_texture_format(TextureFormat format) const override {
        switch (format) {
        case TextureFormat::Rgba8Srgb:
        case TextureFormat::Rgba8Unorm:
            return true;
        case TextureFormat::Bc7Srgb:
        case TextureFormat::Bc7Unorm:
            return m_has_bc7;
        case TextureFormat::Astc4x4Srgb:
        case TextureFormat::Astc4x4Unorm:
            return m_has_astc;
        case TextureFormat::Rg11B10Float:
        case TextureFormat::Depth32Float:
            return false;
        // Half floats filter in OpenGL and OpenGL ES 3.0 and up.
        case TextureFormat::Rgba16Float:
        case TextureFormat::Rgba32Float:
            return true;
        }
        return false;
    }

    // Float color targets are core in OpenGL and in OpenGL ES 3.2, and an extension in ES 3.1.
    bool supports_render_target(TextureFormat format) const override {
        switch (format) {
        case TextureFormat::Rgba8Srgb:
        case TextureFormat::Rgba8Unorm:
        case TextureFormat::Depth32Float:
            return true;
        case TextureFormat::Rgba16Float:
        case TextureFormat::Rg11B10Float:
            return m_has_float_targets;
        default:
            return false;
        }
    }

    std::optional<double> take_gpu_frame_seconds() override {
        collect_frame_timings();
        if (m_gpu_frame_seconds.empty()) {
            return std::nullopt;
        }
        const double seconds = m_gpu_frame_seconds.front();
        m_gpu_frame_seconds.pop_front();
        return seconds;
    }

    core::Result<BufferHandle> create_buffer(BufferUsage /*usage*/,
                                             std::span<const std::byte> contents) override {
        if (contents.empty()) {
            return invalid("A buffer can't be empty");
        }
        if (contents.size() > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())) {
            return invalid("The buffer is too large");
        }
        // Bound as a copy target, which leaves the vertex array's index buffer as it is.
        BufferResource resource;
        m_gl.GenBuffers(1, &resource.id);
        m_gl.BindBuffer(GL_COPY_WRITE_BUFFER, resource.id);
        m_gl.BufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(contents.size()),
                        contents.data(), GL_STATIC_DRAW);
        m_gl.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (m_gl.GetError() == GL_OUT_OF_MEMORY) {
            m_gl.DeleteBuffers(1, &resource.id);
            return failure(std::format("Creating a buffer: {} is out of memory", k_api));
        }
        return handle_cast<BufferTag>(m_buffers.create(resource));
    }

    core::Result<TextureHandle> create_texture(const TextureDesc& desc,
                                               std::span<const std::byte> pixels) override {
        core::Result<TextureResource> texture = upload_texture(desc, pixels);
        if (!texture.has_value()) {
            return texture.error();
        }
        return handle_cast<TextureTag>(m_textures.create(texture.value()));
    }

    // The code is the shader's GLSL text.
    core::Result<ShaderHandle> create_shader(ShaderStage stage,
                                             std::span<const std::byte> code) override {
        const std::string_view text(reinterpret_cast<const char*>(code.data()), code.size());
        if (!text.starts_with(k_shader_version) ||
            (text.size() > k_shader_version.size() && text[k_shader_version.size()] != '\n' &&
             text[k_shader_version.size()] != '\r')) {
            return invalid(std::format("Shader code is not GLSL for {}: it must start with "
                                       "'{}'",
                                       k_api, k_shader_version));
        }

        ShaderResource resource{.stage = stage};
        resource.id = m_gl.CreateShader(stage == ShaderStage::Vertex ? GL_VERTEX_SHADER
                                                                    : GL_FRAGMENT_SHADER);
        const GLchar* const source = text.data();
        const auto length = static_cast<GLint>(text.size());
        m_gl.ShaderSource(resource.id, 1, &source, &length);
        m_gl.CompileShader(resource.id);
        GLint compiled = GL_FALSE;
        m_gl.GetShaderiv(resource.id, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            const std::string log = info_log(resource.id, false);
            m_gl.DeleteShader(resource.id);
            return invalid(std::format("Compiling a {} shader failed: {}",
                                       stage == ShaderStage::Vertex ? "vertex" : "fragment", log));
        }
        return handle_cast<ShaderTag>(m_shaders.create(resource));
    }

    core::Result<PipelineHandle> create_pipeline(const PipelineDesc& desc) override {
        if (!m_was_attached) {
            return invalid("A pipeline needs a surface to have been attached");
        }
        if (const auto problem = pipeline_problem(desc)) {
            return invalid(*problem);
        }
        if (desc.color_target.has_value() && !supports_render_target(*desc.color_target)) {
            return invalid("This device can't draw into the pipeline's color format");
        }
        const ShaderResource* const vertex =
            m_shaders.get(handle_cast<ShaderResource>(desc.vertex_shader));
        const ShaderResource* const fragment =
            m_shaders.get(handle_cast<ShaderResource>(desc.fragment_shader));
        if (vertex == nullptr || vertex->stage != ShaderStage::Vertex ||
            (desc.fragment_shader.is_valid() &&
             (fragment == nullptr || fragment->stage != ShaderStage::Fragment))) {
            return invalid("A pipeline needs valid shaders, one per stage");
        }
        // OpenGL ES links only programs with both stages, so a depth-only pipeline gets an empty
        // fragment shader.
        GLuint fragment_id = fragment != nullptr ? fragment->id : 0;
        if (fragment_id == 0) {
            if (auto empty = empty_fragment_shader(); !empty.has_value()) {
                return empty.error();
            } else {
                fragment_id = empty.value();
            }
        }
        std::vector<VertexAttribute> every_attribute(desc.attributes.begin(), desc.attributes.end());
        every_attribute.insert(every_attribute.end(), desc.instance_attributes.begin(),
                               desc.instance_attributes.end());
        for (const VertexAttribute& attribute : every_attribute) {
            if (attribute.location >= k_max_attributes) {
                return invalid(std::format("Vertex attribute location {} is past the {} that "
                                           "every device has",
                                           attribute.location, k_max_attributes));
            }
        }

        PipelineResource resource;
        resource.program = m_gl.CreateProgram();
        m_gl.AttachShader(resource.program, vertex->id);
        m_gl.AttachShader(resource.program, fragment_id);
        m_gl.LinkProgram(resource.program);
        m_gl.DetachShader(resource.program, vertex->id);
        m_gl.DetachShader(resource.program, fragment_id);
        GLint linked = GL_FALSE;
        m_gl.GetProgramiv(resource.program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            const std::string log = info_log(resource.program, true);
            m_gl.DeleteProgram(resource.program);
            return invalid(std::format("Linking a pipeline's shaders failed: {}", log));
        }
        // A program that reads the wrong bindings or inputs would draw garbage, so it is refused.
        if (auto checked = check_program(resource.program, every_attribute);
            !checked.has_value()) {
            m_gl.DeleteProgram(resource.program);
            return checked.error();
        }

        resource.clip_flip = m_gl.GetUniformLocation(resource.program, k_clip_flip_uniform);
        const bool draws_into_textures =
            desc.color_target.has_value() || desc.depth_target.has_value();
        if (draws_into_textures && resource.clip_flip < 0) {
            m_gl.DeleteProgram(resource.program);
            return invalid(std::format("A pipeline that draws into textures needs a vertex shader "
                                       "that multiplies gl_Position.y by the uniform '{}', as "
                                       "the engine's build compiles them (ADR 0027)",
                                       k_clip_flip_uniform));
        }

        resource.mode = desc.topology == PrimitiveTopology::Lines ? GL_LINES : GL_TRIANGLES;
        resource.vertex_stride = static_cast<GLsizei>(desc.vertex_stride);
        resource.attributes.assign(desc.attributes.begin(), desc.attributes.end());
        resource.instance_stride = static_cast<GLsizei>(desc.instance_stride);
        resource.instance_attributes.assign(desc.instance_attributes.begin(),
                                            desc.instance_attributes.end());
        resource.blend = desc.blend;
        resource.depth_test = desc.depth_test;
        resource.depth_write = desc.depth_test && desc.depth_write;
        resource.cull = desc.cull;
        resource.depth_bias = desc.depth_bias;
        resource.slope_depth_bias = desc.slope_depth_bias;
        resource.color_target = desc.color_target;
        resource.depth_target = desc.depth_target;
        return handle_cast<PipelineTag>(m_pipelines.create(std::move(resource)));
    }

    // OpenGL keeps what the GPU still uses alive itself, so nothing waits here.
    void destroy_buffer(BufferHandle buffer) override {
        if (BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(buffer))) {
            m_gl.DeleteBuffers(1, &resource->id);
            m_buffers.destroy(handle_cast<BufferResource>(buffer));
        }
    }

    void destroy_texture(TextureHandle texture) override {
        if (TextureResource* const resource =
                m_textures.get(handle_cast<TextureResource>(texture))) {
            delete_texture(*resource);
            m_textures.destroy(handle_cast<TextureResource>(texture));
        }
    }

    void destroy_shader(ShaderHandle shader) override {
        if (ShaderResource* const resource = m_shaders.get(handle_cast<ShaderResource>(shader))) {
            m_gl.DeleteShader(resource->id);
            m_shaders.destroy(handle_cast<ShaderResource>(shader));
        }
    }

    void destroy_pipeline(PipelineHandle pipeline) override {
        if (PipelineResource* const resource =
                m_pipelines.get(handle_cast<PipelineResource>(pipeline))) {
            if (m_bound_program == resource->program) {
                m_bound_program = 0;
            }
            m_gl.DeleteProgram(resource->program);
            m_pipelines.destroy(handle_cast<PipelineResource>(pipeline));
        }
    }

    core::Result<CommandList*> begin_frame() override {
        DREAM_VERIFY(!m_in_frame, "begin_frame called twice");
        const auto no_frame = static_cast<CommandList*>(nullptr);
        if (!m_context.has_window()) {
            return no_frame;
        }
        // A minimized window has no size, so its frames are skipped until it gets one again.
        m_extent = m_context.window_size();
        if (m_requested_size.width == 0 || m_requested_size.height == 0 || m_extent.width == 0 ||
            m_extent.height == 0) {
            return no_frame;
        }

        if (!m_window_encodes_srgb) {
            if (auto target = prepare_offscreen_target(); !target.has_value()) {
                return target.error();
            }
        }

        m_in_frame = true;
        m_frame_error.reset();
        // Fresh storage for this frame's vertices and constants; the driver keeps the old storage
        // until the GPU is done with it.
        m_gl.BindBuffer(GL_ARRAY_BUFFER, m_frame_buffer);
        m_gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(k_max_frame_vertex_bytes),
                        nullptr, GL_STREAM_DRAW);
        m_gl.BindBuffer(GL_ARRAY_BUFFER, 0);
        renew_uniform_ring();
        m_frame_bytes_used = 0;
        begin_frame_timing();
        return static_cast<CommandList*>(this);
    }

    core::Result<void> end_frame() override {
        DREAM_VERIFY(m_in_frame, "end_frame called without begin_frame");
        DREAM_VERIFY(!m_in_pass, "The render pass was not ended");
        m_in_frame = false;
        if (!m_window_encodes_srgb) {
            copy_offscreen_target_to_window();
        }
        // Before presenting, so waiting for the display isn't part of the GPU's time.
        end_frame_timing();
        if (m_capture_requested) {
            m_capture_requested = false;
            capture_back_buffer();
        }
        if (const GLenum error = m_gl.GetError(); error == GL_OUT_OF_MEMORY) {
            return failure(std::format("Drawing a frame: {} is out of memory", k_api));
        }
        if (auto presented = m_context.present(); !presented.has_value()) {
            return presented;
        }
        if (m_frame_error.has_value()) {
            return *m_frame_error;
        }
        return {};
    }

    bool capture_next_frame() override {
        m_capture_requested = true;
        return true;
    }

    std::optional<CapturedFrame> take_captured_frame() override {
        return std::exchange(m_captured, std::nullopt);
    }

private:
    core::Result<void> load_functions() {
        bool complete = true;
        std::string missing;
#if defined(__ANDROID__)
        // The OpenGL ES library exports every core function.
#define DREAM_GL_LOAD(member, symbol) m_gl.member = &::symbol;
        DREAM_GL_FUNCTIONS(DREAM_GL_LOAD)
#undef DREAM_GL_LOAD
#else
#define DREAM_GL_LOAD(member, symbol)                                                              \
    m_gl.member = reinterpret_cast<decltype(m_gl.member)>(m_context.function(#symbol));            \
    if (m_gl.member == nullptr) {                                                                  \
        complete = false;                                                                          \
        missing = #symbol;                                                                         \
    }
        DREAM_GL_FUNCTIONS(DREAM_GL_LOAD)
#undef DREAM_GL_LOAD
#endif
        for (const char* name : {"glDebugMessageCallback", "glDebugMessageCallbackKHR"}) {
            if (m_gl.DebugMessageCallback == nullptr) {
                m_gl.DebugMessageCallback = reinterpret_cast<decltype(m_gl.DebugMessageCallback)>(
                    m_context.function(name));
            }
        }
        if (!complete) {
            return failure(std::format("The {} driver lacks {}", k_api, missing));
        }
        return {};
    }

    core::Result<void> check_version() {
        GLint major = 0;
        GLint minor = 0;
        m_gl.GetIntegerv(GL_MAJOR_VERSION, &major);
        m_gl.GetIntegerv(GL_MINOR_VERSION, &minor);
        const auto text = [&](GLenum name) {
            const GLubyte* const value = m_gl.GetString(name);
            return value != nullptr ? std::string(reinterpret_cast<const char*>(value))
                                    : std::string();
        };
        DREAM_LOG_INFO("{} device: {} ({})", k_api, text(GL_RENDERER), text(GL_VERSION));
        if (major < k_major_version || (major == k_major_version && minor < k_minor_version)) {
            return failure(std::format("{} {}.{} is needed, and the driver has {}.{}", k_api,
                                       k_major_version, k_minor_version, major, minor));
        }
        m_version = major * 10 + minor;
        return {};
    }

    void read_extensions() {
        GLint count = 0;
        m_gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
        const auto has = [&](std::string_view wanted) {
            for (GLint index = 0; index < count; ++index) {
                const GLubyte* const name =
                    m_gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
                if (name != nullptr && reinterpret_cast<const char*>(name) == wanted) {
                    return true;
                }
            }
            return false;
        };
        // BC7 is core in OpenGL 4.2; ASTC in OpenGL ES 3.2.
        m_has_bc7 = !k_es || has("GL_EXT_texture_compression_bptc") ||
                    has("GL_ARB_texture_compression_bptc");
        m_has_astc = (k_es && m_version >= 32) || has("GL_KHR_texture_compression_astc_ldr");
        m_has_timer_queries = !k_es || has("GL_EXT_disjoint_timer_query");
        m_has_float_targets = !k_es || m_version >= 32 || has("GL_EXT_color_buffer_float");
        m_has_debug = m_gl.DebugMessageCallback != nullptr &&
                      (!k_es || m_version >= 32 || has("GL_KHR_debug"));
    }

    void start_debug_output(bool debug) {
        if (!debug || !m_has_debug) {
            DREAM_LOG_INFO("{} debug output is off", k_api);
            return;
        }
        m_gl.Enable(k_debug_output);
        m_gl.Enable(k_debug_output_synchronous);
        m_gl.DebugMessageCallback(&on_debug_message, nullptr);
        DREAM_LOG_INFO("{} debug output is on", k_api);
    }

    // The sampler, buffers and vertex array that everything shares.
    void create_shared_state() {
        m_gl.GenVertexArrays(1, &m_vertex_array);
        m_gl.BindVertexArray(m_vertex_array);
        // Binding 1 holds per-instance data, which steps once per instance.
        m_gl.VertexBindingDivisor(1, 1);

        // One sampler per slot, as ADR 0027 lists them.
        m_gl.GenSamplers(static_cast<GLsizei>(m_samplers.size()), m_samplers.data());
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            const GLuint sampler = m_samplers[slot];
            const bool repeat = slot_sampler(slot) == SlotSampler::Repeat;
            const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            m_gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                                   repeat ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            m_gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            m_gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
            m_gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
            if (slot == k_shadow_slot) {
                m_gl.SamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE,
                                       GL_COMPARE_REF_TO_TEXTURE);
                m_gl.SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            }
            m_gl.BindSampler(texture_binding(slot), sampler);
        }
        m_gl.ActiveTexture(GL_TEXTURE0 + k_texture_unit);

        GLint alignment = 256;
        m_gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
        m_uniform_alignment = static_cast<std::size_t>(std::max(alignment, 1));
        m_gl.GenBuffers(1, &m_uniform_ring);
        m_gl.GenBuffers(1, &m_frame_buffer);
        m_gl.BindBuffer(GL_UNIFORM_BUFFER, m_uniform_ring);
        m_gl.BufferData(GL_UNIFORM_BUFFER, k_uniform_ring_bytes, nullptr, GL_STREAM_DRAW);
        m_gl.BindBuffer(GL_ARRAY_BUFFER, m_frame_buffer);
        m_gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(k_max_frame_vertex_bytes),
                        nullptr, GL_STREAM_DRAW);
        m_gl.BindBuffer(GL_ARRAY_BUFFER, 0);

        m_gl.FrontFace(GL_CCW); // The engine's clip space has +Y up, as OpenGL's does (ADR 0005).
        m_gl.CullFace(GL_BACK);
        m_gl.DepthFunc(GL_LESS);
        m_gl.BlendEquation(GL_FUNC_ADD);
        m_gl.Enable(GL_SCISSOR_TEST);
        m_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        m_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);

        if (m_has_timer_queries) {
            for (TimingQuery& query : m_timing_queries) {
                m_gl.GenQueries(1, &query.id);
            }
        } else {
            DREAM_LOG_WARNING("{} has no timer queries, so GPU time isn't measured", k_api);
        }
    }

    std::string info_log(GLuint object, bool program) {
        GLint length = 0;
        if (program) {
            m_gl.GetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
        } else {
            m_gl.GetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
        }
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        GLsizei written = 0;
        if (program) {
            m_gl.GetProgramInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
        } else {
            m_gl.GetShaderInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
        }
        log.resize(static_cast<std::size_t>(std::max(written, 0)));
        while (!log.empty() && (log.back() == '\n' || log.back() == '\r')) {
            log.pop_back();
        }
        return log.empty() ? std::string("no details") : log;
    }

    // Checks a linked program against the bindings this backend uses (see the top of the file),
    // and that the pipeline gives every vertex input the shader reads.
    core::Result<void> check_program(GLuint program, std::span<const VertexAttribute> attributes) {
        std::array<GLchar, 256> name{};
        GLint blocks = 0;
        m_gl.GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
        for (GLint block = 0; block < blocks; ++block) {
            const auto index = static_cast<GLuint>(block);
            GLint binding = 0;
            GLint size = 0;
            m_gl.GetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_BINDING, &binding);
            m_gl.GetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
            m_gl.GetActiveUniformBlockName(program, index, static_cast<GLsizei>(name.size()),
                                           nullptr, name.data());
            const std::string_view block_name(name.data());
            const auto bytes = static_cast<std::size_t>(size);
            if (static_cast<GLuint>(binding) == k_uniform_binding) {
                if (bytes > k_max_uniform_size) {
                    return invalid(std::format("Uniform block '{}' is {} bytes; at most {} are "
                                               "allowed",
                                               block_name, bytes, k_max_uniform_size));
                }
            } else if (static_cast<GLuint>(binding) == k_push_constant_binding) {
                if (bytes > k_max_push_constant_size) {
                    return invalid(std::format("Push constants '{}' are {} bytes; at most {} are "
                                               "allowed",
                                               block_name, bytes, k_max_push_constant_size));
                }
            } else if (static_cast<GLuint>(binding) == k_draw_block_binding) {
                if (bytes > k_max_draw_uniform_size) {
                    return invalid(std::format("Per-draw block '{}' is {} bytes; at most {} are "
                                               "allowed",
                                               block_name, bytes, k_max_draw_uniform_size));
                }
            } else {
                return invalid(std::format("Uniform block '{}' is at binding {}; {} shaders must "
                                           "put the uniform block at binding {}, push constants "
                                           "at binding {} and the per-draw block at binding {}, "
                                           "see ADR 0026 and 0036",
                                           block_name, binding, k_api, k_uniform_binding,
                                           k_push_constant_binding, k_draw_block_binding));
            }
        }

        GLint uniforms = 0;
        m_gl.GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniforms);
        for (GLint uniform = 0; uniform < uniforms; ++uniform) {
            GLint count = 0;
            GLenum type = 0;
            m_gl.GetActiveUniform(program, static_cast<GLuint>(uniform),
                                  static_cast<GLsizei>(name.size()), nullptr, &count, &type,
                                  name.data());
            if (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW) {
                continue; // Members of uniform blocks, checked above, and the clip flip.
            }
            GLint unit = -1;
            m_gl.GetUniformiv(program, m_gl.GetUniformLocation(program, name.data()), &unit);
            const GLuint shadow_unit = texture_binding(k_shadow_slot);
            const bool shadow = type == GL_SAMPLER_2D_SHADOW;
            bool on_a_slot = false;
            for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
                on_a_slot = on_a_slot || static_cast<GLint>(texture_binding(slot)) == unit;
            }
            const bool placed = count == 1 && on_a_slot &&
                                (static_cast<GLuint>(unit) == shadow_unit) == shadow;
            if (!placed) {
                return invalid(std::format("Texture '{}' is at binding {}; textures go at bindings "
                                           "{} to {} and {}, and shadow samplers at {} alone, see "
                                           "ADR 0027",
                                           name.data(), unit, texture_binding(0),
                                           texture_binding(k_texture_slots - 2),
                                           texture_binding(k_texture_slots - 1), shadow_unit));
            }
        }

        GLint inputs = 0;
        m_gl.GetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &inputs);
        for (GLint input = 0; input < inputs; ++input) {
            GLint count = 0;
            GLenum type = 0;
            m_gl.GetActiveAttrib(program, static_cast<GLuint>(input),
                                 static_cast<GLsizei>(name.size()), nullptr, &count, &type,
                                 name.data());
            const std::string_view input_name(name.data());
            if (input_name.starts_with("gl_")) {
                continue;
            }
            const GLint location = m_gl.GetAttribLocation(program, name.data());
            const bool provided = std::ranges::any_of(attributes, [&](const VertexAttribute& a) {
                return static_cast<GLint>(a.location) == location;
            });
            if (!provided) {
                return invalid(std::format("The vertex shader reads input {} ({}), which the "
                                           "pipeline has no attribute for",
                                           location, input_name));
            }
        }
        return {};
    }

    core::Result<TextureResource> upload_texture(const TextureDesc& desc,
                                                 std::span<const std::byte> pixels) {
        if (const auto problem = texture_problem(desc, pixels.size())) {
            return invalid(*problem);
        }
        if (desc.render_target ? !supports_render_target(desc.format)
                               : !supports_texture_format(desc.format)) {
            return invalid(desc.render_target
                               ? "This device can't draw into textures of this format"
                               : "This device doesn't support the texture's format");
        }

        const bool mipmaps = generates_mipmaps(desc);
        const std::uint32_t levels =
            mipmaps ? full_mip_chain(desc.width, desc.height) : desc.mip_levels;
        const GLenum format = internal_format(desc.format);

        TextureResource texture{.format = desc.format,
                                .size = {desc.width, desc.height},
                                .render_target = desc.render_target,
                                .dynamic = desc.dynamic && !desc.render_target};
        m_gl.GenTextures(1, &texture.id);
        bind_on_unit(0, texture.id);
        m_gl.TexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels), format,
                          static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));

        // Each level's rows, packed one after another, as the pixels hold them. The first row is
        // the top of the image, and OpenGL's texture coordinates start at the first row, so the
        // shaders' coordinates mean the same as on the other backends.
        std::size_t offset = 0;
        for (std::uint32_t level = 0; level < (desc.render_target ? 0u : desc.mip_levels);
             ++level) {
            const std::uint32_t width = mip_size(desc.width, level);
            const std::uint32_t height = mip_size(desc.height, level);
            const std::size_t bytes = texture_level_bytes(desc.format, width, height);
            if (is_compressed(desc.format)) {
                m_gl.CompressedTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                                             static_cast<GLsizei>(width),
                                             static_cast<GLsizei>(height), format,
                                             static_cast<GLsizei>(bytes), pixels.data() + offset);
            } else {
                m_gl.TexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                   GL_RGBA, pixel_type(desc.format), pixels.data() + offset);
            }
            offset += bytes;
        }
        if (mipmaps) {
            m_gl.GenerateMipmap(GL_TEXTURE_2D);
        }
        if (desc.format == TextureFormat::Rgba32Float) {
            // OpenGL ES can't filter full floats, and a texture that would be filtered is
            // incomplete, so even texelFetch reads nothing. It is read without a sampler object,
            // by these settings of its own.
            m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        bind_on_unit(0, m_bound_textures[0]);

        if (const GLenum error = m_gl.GetError(); error != GL_NO_ERROR) {
            m_gl.DeleteTextures(1, &texture.id);
            return failure(std::format("Creating a texture failed with {} error 0x{:04X}", k_api,
                                       static_cast<unsigned>(error)));
        }
        return texture;
    }

    // Clears one pixel to a linear gray of 0.2, which a window that encodes sRGB stores as about
    // 124, and one that doesn't as 51.
    bool window_encodes_srgb() {
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        m_gl.Scissor(0, 0, 1, 1);
        m_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.ClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        m_gl.Clear(GL_COLOR_BUFFER_BIT);
        std::array<GLubyte, 4> pixel{};
        m_gl.ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return pixel[0] > 90;
    }

    // An sRGB color texture and a depth buffer the size of the window, made again when it
    // changes.
    core::Result<void> prepare_offscreen_target() {
        if (m_offscreen_framebuffer != 0 && m_offscreen_size == m_extent) {
            return {};
        }
        delete_offscreen_target();
        const auto width = static_cast<GLsizei>(m_extent.width);
        const auto height = static_cast<GLsizei>(m_extent.height);

        m_gl.GenTextures(1, &m_offscreen_color);
        m_gl.BindTexture(GL_TEXTURE_2D, m_offscreen_color);
        m_gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, width, height);
        m_gl.BindTexture(GL_TEXTURE_2D, m_bound_textures[0]);
        m_gl.GenRenderbuffers(1, &m_offscreen_depth);
        m_gl.BindRenderbuffer(GL_RENDERBUFFER, m_offscreen_depth);
        m_gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        m_gl.BindRenderbuffer(GL_RENDERBUFFER, 0);

        m_gl.GenFramebuffers(1, &m_offscreen_framebuffer);
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, m_offscreen_framebuffer);
        m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                  m_offscreen_color, 0);
        m_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                     m_offscreen_depth);
        const GLenum status = m_gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            delete_offscreen_target();
            return failure(std::format("The offscreen target is incomplete ({} status 0x{:04X})",
                                       k_api, static_cast<unsigned>(status)));
        }
        m_offscreen_size = m_extent;
        return {};
    }

    void delete_offscreen_target() {
        if (m_offscreen_framebuffer != 0) {
            m_gl.DeleteFramebuffers(1, &m_offscreen_framebuffer);
        }
        if (m_offscreen_depth != 0) {
            m_gl.DeleteRenderbuffers(1, &m_offscreen_depth);
        }
        if (m_offscreen_color != 0) {
            m_gl.DeleteTextures(1, &m_offscreen_color);
        }
        m_offscreen_framebuffer = 0;
        m_offscreen_depth = 0;
        m_offscreen_color = 0;
        m_offscreen_size = {};
    }

    // Draws the offscreen target over the window with one triangle that covers it. Sampling the
    // sRGB texture decodes it, so the shader encodes again, which the window doesn't.
    void copy_offscreen_target_to_window() {
        if (m_copy_program == 0) {
            if (auto created = create_copy_program(); !created.has_value()) {
                if (!m_frame_error.has_value()) {
                    m_frame_error = created.error();
                }
                return;
            }
        }
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        m_gl.Viewport(0, 0, static_cast<GLsizei>(m_extent.width),
                      static_cast<GLsizei>(m_extent.height));
        m_gl.Scissor(0, 0, static_cast<GLsizei>(m_extent.width),
                     static_cast<GLsizei>(m_extent.height));
        m_gl.Disable(GL_BLEND);
        m_gl.Disable(GL_DEPTH_TEST);
        m_gl.Disable(GL_CULL_FACE);
        m_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (GLuint location = 0; location < k_max_attributes; ++location) {
            if ((m_enabled_attributes & (1u << location)) != 0) {
                m_gl.DisableVertexAttribArray(location);
            }
        }
        m_enabled_attributes = 0;
        m_gl.UseProgram(m_copy_program);
        m_bound_program = m_copy_program;
        m_gl.ActiveTexture(GL_TEXTURE0);
        m_gl.BindTexture(GL_TEXTURE_2D, m_offscreen_color);
#if defined(_WIN32)
        // The shader encodes, so the window mustn't, whatever the probe saw of its clears.
        m_gl.Disable(k_framebuffer_srgb);
#endif
        m_gl.DrawArrays(GL_TRIANGLES, 0, 3);
#if defined(_WIN32)
        m_gl.Enable(k_framebuffer_srgb);
        static_cast<void>(m_gl.GetError());
#endif
        m_gl.BindTexture(GL_TEXTURE_2D, 0);
        m_gl.ActiveTexture(GL_TEXTURE0 + k_texture_unit);
    }

    core::Result<void> create_copy_program() {
        static constexpr std::string_view k_vertex = R"glsl(
const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)glsl";
        static constexpr std::string_view k_fragment = R"glsl(
precision highp float;
layout(binding = 0) uniform highp sampler2D frame;
layout(location = 0) out vec4 color;
vec3 encode(vec3 linear) {
    return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), linear));
}
void main() {
    vec4 value = texelFetch(frame, ivec2(gl_FragCoord.xy), 0);
    color = vec4(encode(value.rgb), value.a);
}
)glsl";
        GLuint shaders[2] = {0, 0};
        const std::string_view bodies[2] = {k_vertex, k_fragment};
        const GLenum stages[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        for (int index = 0; index < 2; ++index) {
            const std::string source = std::format("{}\n{}", k_shader_version, bodies[index]);
            const GLchar* const text = source.c_str();
            shaders[index] = m_gl.CreateShader(stages[index]);
            m_gl.ShaderSource(shaders[index], 1, &text, nullptr);
            m_gl.CompileShader(shaders[index]);
        }
        m_copy_program = m_gl.CreateProgram();
        m_gl.AttachShader(m_copy_program, shaders[0]);
        m_gl.AttachShader(m_copy_program, shaders[1]);
        m_gl.LinkProgram(m_copy_program);
        m_gl.DetachShader(m_copy_program, shaders[0]);
        m_gl.DetachShader(m_copy_program, shaders[1]);
        m_gl.DeleteShader(shaders[0]);
        m_gl.DeleteShader(shaders[1]);
        GLint linked = GL_FALSE;
        m_gl.GetProgramiv(m_copy_program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            const std::string log = info_log(m_copy_program, true);
            m_gl.DeleteProgram(m_copy_program);
            m_copy_program = 0;
            return failure(std::format("Making the program that copies frames to the window "
                                       "failed: {}",
                                       log));
        }
        return {};
    }

    void delete_texture(TextureResource& texture) {
        if (texture.id == 0) {
            return;
        }
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            if (m_bound_textures[slot] == texture.id) {
                bind_on_unit(slot, 0);
                m_bound_textures[slot] = 0;
            }
        }
        std::erase_if(m_target_framebuffers, [&](const TargetFramebuffer& target) {
            if (target.color != texture.id && target.depth != texture.id) {
                return false;
            }
            m_gl.DeleteFramebuffers(1, &target.framebuffer);
            return true;
        });
        m_gl.DeleteTextures(1, &texture.id);
        texture.id = 0;
    }

    // Binds a texture on a slot's unit, leaving slot 0's unit active, as the rest of the device
    // expects.
    void bind_on_unit(std::uint32_t slot, GLuint id) {
        if (slot != 0) {
            m_gl.ActiveTexture(GL_TEXTURE0 + texture_binding(slot));
        }
        m_gl.BindTexture(GL_TEXTURE_2D, id);
        if (slot != 0) {
            m_gl.ActiveTexture(GL_TEXTURE0 + k_texture_unit);
        }
    }

    core::Result<GLuint> empty_fragment_shader() {
        if (m_empty_fragment_shader != 0) {
            return m_empty_fragment_shader;
        }
        const std::string source = std::format("{}\nvoid main() {{}}\n", k_shader_version);
        const GLchar* const text = source.c_str();
        const GLuint shader = m_gl.CreateShader(GL_FRAGMENT_SHADER);
        m_gl.ShaderSource(shader, 1, &text, nullptr);
        m_gl.CompileShader(shader);
        GLint compiled = GL_FALSE;
        m_gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            const std::string log = info_log(shader, false);
            m_gl.DeleteShader(shader);
            return failure(std::format("Compiling the empty fragment shader failed: {}", log));
        }
        m_empty_fragment_shader = shader;
        return shader;
    }

    // The framebuffer object for a pair of targets, made the first time they are drawn into.
    core::Result<GLuint> target_framebuffer(const TextureResource* color,
                                            const TextureResource* depth) {
        const GLuint color_id = color != nullptr ? color->id : 0;
        const GLuint depth_id = depth != nullptr ? depth->id : 0;
        for (const TargetFramebuffer& target : m_target_framebuffers) {
            if (target.color == color_id && target.depth == depth_id) {
                return target.framebuffer;
            }
        }
        TargetFramebuffer target{.color = color_id, .depth = depth_id};
        m_gl.GenFramebuffers(1, &target.framebuffer);
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        if (color_id != 0) {
            m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                      color_id, 0);
        }
        if (depth_id != 0) {
            m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                                      depth_id, 0);
        }
        const GLenum status = m_gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            m_gl.DeleteFramebuffers(1, &target.framebuffer);
            return failure(std::format("The render targets can't be drawn into together ({} "
                                       "status 0x{:04X})",
                                       k_api, static_cast<unsigned>(status)));
        }
        m_target_framebuffers.push_back(target);
        return target.framebuffer;
    }

    // GPU timing, for take_gpu_frame_seconds: a ring of time-elapsed queries, one around each
    // frame's commands, read back some frames later without waiting. A frame goes untimed if every
    // query is still waiting for the GPU.
    void begin_frame_timing() {
        collect_frame_timings();
        m_timing_this_frame = m_has_timer_queries && m_timing_pending < m_timing_queries.size();
        if (m_timing_this_frame) {
            m_gl.BeginQuery(k_time_elapsed, next_timing_query().id);
        }
    }

    void end_frame_timing() {
        if (!m_timing_this_frame) {
            return;
        }
        m_gl.EndQuery(k_time_elapsed);
        ++m_timing_pending;
        m_timing_this_frame = false;
    }

    TimingQuery& next_timing_query() {
        return m_timing_queries[(m_timing_oldest + m_timing_pending) % m_timing_queries.size()];
    }

    void collect_frame_timings() {
        if (m_timing_pending == 0) {
            return;
        }
        // On OpenGL ES, a disjoint event, such as the GPU changing its clock, spoils every query
        // in flight.
        bool disjoint = false;
        if constexpr (k_es) {
            GLint value = 0;
            m_gl.GetIntegerv(k_gpu_disjoint, &value);
            disjoint = value != 0;
        }
        while (m_timing_pending > 0) {
            const TimingQuery& query = m_timing_queries[m_timing_oldest];
            GLuint available = GL_FALSE;
            m_gl.GetQueryObjectuiv(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available == GL_FALSE) {
                return;
            }
            // In nanoseconds. 32 bits hold more than 4 seconds, far longer than any frame.
            GLuint nanoseconds = 0;
            m_gl.GetQueryObjectuiv(query.id, GL_QUERY_RESULT, &nanoseconds);
            if (!disjoint) {
                if (m_gpu_frame_seconds.size() == k_kept_gpu_frame_times) {
                    m_gpu_frame_seconds.pop_front();
                }
                m_gpu_frame_seconds.push_back(static_cast<double>(nanoseconds) * 1e-9);
            }
            m_timing_oldest = (m_timing_oldest + 1) % m_timing_queries.size();
            --m_timing_pending;
        }
    }

    // Reads the frame about to be presented, bottom row first as OpenGL gives it, into m_captured,
    // top row first.
    void capture_back_buffer() {
        const std::uint32_t width = m_extent.width;
        const std::uint32_t height = m_extent.height;
        std::vector<std::byte> bottom_up(std::size_t{width} * height * 4);
        m_gl.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                        GL_UNSIGNED_BYTE, bottom_up.data());
        if (const GLenum error = m_gl.GetError(); error != GL_NO_ERROR) {
            DREAM_LOG_WARNING("Could not capture a frame: {} error 0x{:04X}", k_api,
                              static_cast<unsigned>(error));
            return;
        }
        CapturedFrame frame{.width = width, .height = height};
        frame.pixels.resize(bottom_up.size());
        const std::size_t row_bytes = std::size_t{width} * 4;
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(frame.pixels.data() + std::size_t{y} * row_bytes,
                        bottom_up.data() + std::size_t{height - 1 - y} * row_bytes, row_bytes);
        }
        for (std::size_t pixel = 3; pixel < frame.pixels.size(); pixel += 4) {
            frame.pixels[pixel] = std::byte{255};
        }
        m_captured = std::move(frame);
    }

    // Objects still alive when the device closes were never destroyed, which over a long session
    // or repeated loads is a leak. The device deletes them afterwards.
    void warn_about_leftovers() const {
        if (m_textures.size() + m_buffers.size() + m_shaders.size() + m_pipelines.size() > 0) {
            DREAM_LOG_WARNING("The graphics device closed with objects the game never destroyed: "
                              "{} textures, {} buffers, {} shaders and {} pipelines",
                              m_textures.size(), m_buffers.size(), m_shaders.size(),
                              m_pipelines.size());
        }
    }

    void renew_uniform_ring() {
        m_gl.BindBuffer(GL_UNIFORM_BUFFER, m_uniform_ring);
        m_gl.BufferData(GL_UNIFORM_BUFFER, k_uniform_ring_bytes, nullptr, GL_STREAM_DRAW);
        m_uniform_offset = 0;
    }

    // Copies a block of constants into a slot of the ring, a whole `slot_bytes` long, since the
    // shader's block may be larger than what was given, and binds it at `binding`.
    void write_constants(GLuint binding, std::span<const std::byte> data, std::size_t slot_bytes) {
        std::size_t offset = (m_uniform_offset + m_uniform_alignment - 1) / m_uniform_alignment *
                             m_uniform_alignment;
        if (offset + slot_bytes > static_cast<std::size_t>(k_uniform_ring_bytes)) {
            renew_uniform_ring();
            offset = 0;
        }
        m_gl.BindBuffer(GL_UNIFORM_BUFFER, m_uniform_ring);
        if (!data.empty()) {
            m_gl.BufferSubData(GL_UNIFORM_BUFFER, static_cast<GLintptr>(offset),
                               static_cast<GLsizeiptr>(data.size()), data.data());
        }
        m_gl.BindBufferRange(GL_UNIFORM_BUFFER, binding, m_uniform_ring,
                             static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(slot_bytes));
        m_uniform_offset = offset + slot_bytes;
    }

    // Copies data into this frame's vertex memory, four-byte aligned. Returns the offset, or
    // nothing if it doesn't fit.
    std::optional<std::size_t> copy_to_frame(std::span<const std::byte> data) {
        const std::size_t offset = (m_frame_bytes_used + 3) & ~std::size_t{3};
        if (offset + data.size() > k_max_frame_vertex_bytes) {
            return std::nullopt;
        }
        if (!data.empty()) {
            m_gl.BindBuffer(GL_COPY_WRITE_BUFFER, m_frame_buffer);
            m_gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset),
                               static_cast<GLsizeiptr>(data.size()), data.data());
            m_gl.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }
        m_frame_bytes_used = offset + data.size();
        return offset;
    }

    // The vertex stride belongs to the pipeline, so the buffer is bound just before drawing, once
    // both are known.
    void flush_vertex_buffer() {
        if (!m_vertex_buffer_dirty) {
            return;
        }
        m_vertex_buffer_dirty = false;
        m_gl.BindVertexBuffer(0, m_vertex_buffer, static_cast<GLintptr>(m_vertex_buffer_offset),
                              m_bound_stride);
    }

    // CommandList

    void begin_render_pass(const ClearColor& color) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, m_window_encodes_srgb ? 0 : m_offscreen_framebuffer);
        start_pass(m_extent, false, {}, {});
        m_gl.ClearColor(color.r, color.g, color.b, color.a);
        m_gl.ClearDepthf(1.0f);
        m_gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void begin_render_pass(const RenderTargets& targets) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        const TextureResource* const color =
            m_textures.get(handle_cast<TextureResource>(targets.color));
        const TextureResource* const depth =
            m_textures.get(handle_cast<TextureResource>(targets.depth));
        DREAM_VERIFY(color != nullptr || depth != nullptr, "A render pass needs a target");
        DREAM_VERIFY(color == nullptr || (color->render_target && !is_depth(color->format)),
                     "The color target must be a color render target");
        DREAM_VERIFY(depth == nullptr || (depth->render_target && is_depth(depth->format)),
                     "The depth target must be a depth render target");
        const platform::SurfaceSize size = color != nullptr ? color->size : depth->size;
        DREAM_VERIFY(color == nullptr || depth == nullptr || color->size == depth->size,
                     "A pass's targets must be the same size");

        const core::Result<GLuint> framebuffer = target_framebuffer(color, depth);
        if (!framebuffer.has_value()) {
            if (!m_frame_error.has_value()) {
                m_frame_error = framebuffer.error();
            }
            // Drawing goes nowhere rather than into the window.
            m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            m_gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer.value());
        }
        start_pass(size, true, color != nullptr ? std::optional(color->format) : std::nullopt,
                   depth != nullptr ? std::optional(depth->format) : std::nullopt);
        m_pass_textures = {color != nullptr ? color->id : 0, depth != nullptr ? depth->id : 0};

        GLbitfield clear = 0;
        if (color != nullptr && targets.clear_color.has_value()) {
            const ClearColor& value = *targets.clear_color;
            m_gl.ClearColor(value.r, value.g, value.b, value.a);
            clear |= GL_COLOR_BUFFER_BIT;
        }
        if (depth != nullptr && targets.clear_depth.has_value()) {
            m_gl.ClearDepthf(*targets.clear_depth);
            clear |= GL_DEPTH_BUFFER_BIT;
        }
        if (clear != 0) {
            m_gl.Clear(clear);
        }
    }

    // What every pass starts with. A pass into textures draws upside down, with the winding of
    // its triangles reversed to match, so their rows are stored from the top, as on the other APIs.
    void start_pass(platform::SurfaceSize size, bool into_textures,
                    std::optional<TextureFormat> color, std::optional<TextureFormat> depth) {
        m_in_pass = true;
        m_pass_size = size;
        m_pass_into_textures = into_textures;
        m_pass_color = color;
        m_pass_depth = depth;
        m_pass_textures = {0, 0};
        m_gl.FrontFace(into_textures ? GL_CW : GL_CCW);
        m_gl.Viewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
        set_scissor(std::nullopt);
        // Clearing obeys the write masks, which the last pipeline may have turned off.
        m_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.DepthMask(GL_TRUE);
        m_depth_write = true;

        // Zeroed constants until the game sets some, so every block is bound to something.
        write_constants(k_uniform_binding, {}, k_max_uniform_size);
        write_constants(k_push_constant_binding, {}, k_max_push_constant_size);
        write_constants(k_draw_block_binding, {}, k_max_draw_uniform_size);
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            bind_texture(slot, TextureHandle{});
        }
    }

    void end_render_pass() override {
        DREAM_VERIFY(m_in_pass, "No render pass to end");
        m_in_pass = false;
        if (m_pass_into_textures) {
            m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    void bind_pipeline(PipelineHandle handle) override {
        const PipelineResource* const resource =
            m_pipelines.get(handle_cast<PipelineResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a pipeline that doesn't exist");
        DREAM_VERIFY(resource->color_target == m_pass_color &&
                         resource->depth_target == m_pass_depth,
                     "The pipeline was made for other render targets than this pass has");

        m_gl.UseProgram(resource->program);
        m_bound_program = resource->program;
        m_mode = resource->mode;
        if (resource->clip_flip >= 0) {
            m_gl.Uniform1f(resource->clip_flip, m_pass_into_textures ? -1.0f : 1.0f);
        }

        std::uint32_t enabled = 0;
        for (const VertexAttribute& attribute : resource->attributes) {
            const VertexFormatInfo format = to_gl(attribute.format);
            m_gl.VertexAttribFormat(attribute.location, format.size, format.type,
                                    format.normalized, attribute.offset);
            m_gl.VertexAttribBinding(attribute.location, 0);
            enabled |= 1u << attribute.location;
        }
        for (const VertexAttribute& attribute : resource->instance_attributes) {
            const VertexFormatInfo format = to_gl(attribute.format);
            m_gl.VertexAttribFormat(attribute.location, format.size, format.type,
                                    format.normalized, attribute.offset);
            m_gl.VertexAttribBinding(attribute.location, 1);
            enabled |= 1u << attribute.location;
        }
        m_bound_instance_stride = resource->instance_stride;
        for (GLuint location = 0; location < k_max_attributes; ++location) {
            const std::uint32_t bit = 1u << location;
            if ((enabled & bit) != 0 && (m_enabled_attributes & bit) == 0) {
                m_gl.EnableVertexAttribArray(location);
            } else if ((enabled & bit) == 0 && (m_enabled_attributes & bit) != 0) {
                m_gl.DisableVertexAttribArray(location);
            }
        }
        m_enabled_attributes = enabled;

        if (resource->cull == CullMode::None) {
            m_gl.Disable(GL_CULL_FACE);
        } else {
            m_gl.Enable(GL_CULL_FACE);
            m_gl.CullFace(resource->cull == CullMode::Back ? GL_BACK : GL_FRONT);
        }
        if (resource->depth_bias != 0.0f || resource->slope_depth_bias != 0.0f) {
            m_gl.Enable(GL_POLYGON_OFFSET_FILL);
            m_gl.PolygonOffset(resource->slope_depth_bias, resource->depth_bias);
        } else {
            m_gl.Disable(GL_POLYGON_OFFSET_FILL);
        }
        if (resource->depth_test) {
            m_gl.Enable(GL_DEPTH_TEST);
        } else {
            m_gl.Disable(GL_DEPTH_TEST);
        }
        if (resource->depth_write != m_depth_write) {
            m_gl.DepthMask(resource->depth_write ? GL_TRUE : GL_FALSE);
            m_depth_write = resource->depth_write;
        }
        if (resource->blend == BlendMode::Opaque) {
            m_gl.Disable(GL_BLEND);
        } else {
            m_gl.Enable(GL_BLEND);
            m_gl.BlendFuncSeparate(resource->blend == BlendMode::Alpha ? GL_SRC_ALPHA : GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        if (resource->vertex_stride != m_bound_stride) {
            m_bound_stride = resource->vertex_stride;
            m_vertex_buffer_dirty = true;
        }
    }

    void bind_vertex_buffer(BufferHandle handle) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        m_vertex_buffer = resource->id;
        m_vertex_buffer_offset = 0;
        m_vertex_buffer_dirty = true;
    }

    void bind_index_buffer(BufferHandle handle, IndexFormat format) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        m_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, resource->id);
        m_index_type = format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        m_index_offset = 0;
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
        write_constants(k_push_constant_binding, data, k_max_push_constant_size);
    }

    void set_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_uniform_size, "Too many uniforms");
        write_constants(k_uniform_binding, data, k_max_uniform_size);
    }

    void set_draw_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_draw_uniform_size, "Too many per-draw uniforms");
        write_constants(k_draw_block_binding, data, k_max_draw_uniform_size);
    }

    void bind_texture(std::uint32_t slot, TextureHandle handle) override {
        DREAM_VERIFY(slot < k_texture_slots, "There is no such texture slot");
        GLuint id = slot == k_shadow_slot ? m_far_depth_texture.id : m_white_texture.id;
        bool unfiltered = false;
        if (handle.is_valid()) {
            const TextureResource* const resource =
                m_textures.get(handle_cast<TextureResource>(handle));
            DREAM_VERIFY(resource != nullptr, "Binding a texture that doesn't exist");
            DREAM_VERIFY((slot == k_shadow_slot) == is_depth(resource->format),
                         "The shadow slot takes depth textures, and only it");
            DREAM_VERIFY(resource->id != m_pass_textures[0] && resource->id != m_pass_textures[1],
                         "A pass can't read the textures it draws into");
            id = resource->id;
            unfiltered = resource->format == TextureFormat::Rgba32Float;
        }
        bind_on_unit(slot, id);
        m_bound_textures[slot] = id;
        // A full-float texture is read by its own nearest settings, not the slot's sampler.
        if (unfiltered != m_unfiltered_slots[slot]) {
            m_gl.BindSampler(texture_binding(slot), unfiltered ? 0 : m_samplers[slot]);
            m_unfiltered_slots[slot] = unfiltered;
        }
    }

    void update_texture(TextureHandle handle, std::uint32_t first_row,
                        std::span<const std::byte> rows) override {
        DREAM_VERIFY(!m_in_pass, "Textures are updated between render passes");
        const TextureResource* const texture = m_textures.get(handle_cast<TextureResource>(handle));
        DREAM_VERIFY(texture != nullptr && texture->dynamic, "Updating a texture that isn't dynamic");
        const std::size_t row_bytes = texture_level_bytes(texture->format, texture->size.width, 1);
        const std::size_t row_count = rows.size() / row_bytes;
        DREAM_VERIFY(row_count * row_bytes == rows.size() &&
                         first_row + row_count <= texture->size.height,
                     "update_texture takes whole rows within the texture");
        if (row_count == 0) {
            return;
        }
        bind_on_unit(0, texture->id);
        m_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(first_row),
                           static_cast<GLsizei>(texture->size.width),
                           static_cast<GLsizei>(row_count), GL_RGBA, pixel_type(texture->format),
                           rows.data());
        bind_on_unit(0, m_bound_textures[0]);
    }

    void draw_indexed(std::uint32_t index_count) override {
        draw_indexed_range(0, index_count);
    }

    void draw_indexed_range(std::uint32_t first_index, std::uint32_t index_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        flush_vertex_buffer();
        const std::size_t index_size = m_index_type == GL_UNSIGNED_SHORT ? 2 : 4;
        const std::uintptr_t start = m_index_offset + std::uintptr_t{first_index} * index_size;
        m_gl.DrawElements(m_mode, static_cast<GLsizei>(index_count), m_index_type,
                          reinterpret_cast<const void*>(start));
    }

    bool draw_vertices(std::span<const std::byte> vertices, std::uint32_t vertex_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        DREAM_VERIFY(vertex_count == 0 || vertices.size() % vertex_count == 0,
                     "The vertex data doesn't divide into vertex_count vertices");
        if (vertex_count == 0) {
            return true;
        }
        const std::optional<std::size_t> offset = copy_to_frame(vertices);
        if (!offset.has_value()) {
            return false;
        }
        m_vertex_buffer = m_frame_buffer;
        m_vertex_buffer_offset = *offset;
        m_vertex_buffer_dirty = true;
        flush_vertex_buffer();
        m_gl.DrawArrays(m_mode, 0, static_cast<GLsizei>(vertex_count));
        return true;
    }

    bool bind_frame_geometry(std::span<const std::byte> vertices,
                             std::span<const std::uint32_t> indices) override {
        DREAM_VERIFY(m_in_pass, "Binding geometry outside a render pass");
        const std::size_t start = m_frame_bytes_used;
        const std::optional<std::size_t> vertex_offset = copy_to_frame(vertices);
        const std::optional<std::size_t> index_offset =
            vertex_offset.has_value() ? copy_to_frame(std::as_bytes(indices)) : std::nullopt;
        if (!index_offset.has_value()) {
            m_frame_bytes_used = start;
            return false;
        }
        m_vertex_buffer = m_frame_buffer;
        m_vertex_buffer_offset = *vertex_offset;
        m_vertex_buffer_dirty = true;
        m_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_frame_buffer);
        m_index_type = GL_UNSIGNED_INT;
        m_index_offset = *index_offset;
        return true;
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        DREAM_VERIFY(m_in_pass, "Binding instances outside a render pass");
        const std::optional<std::size_t> offset = copy_to_frame(instances);
        if (!offset.has_value()) {
            return false;
        }
        m_instance_offset = *offset;
        return true;
    }

    void draw_indexed_instanced(std::uint32_t first_index, std::uint32_t index_count,
                                std::uint32_t instance_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        DREAM_VERIFY(m_bound_instance_stride > 0, "Drawing instances with a pipeline that has none");
        flush_vertex_buffer();
        // Set with each draw, since the stride comes from the pipeline bound now.
        m_gl.BindVertexBuffer(1, m_frame_buffer, static_cast<GLintptr>(m_instance_offset),
                              m_bound_instance_stride);
        const std::size_t index_size = m_index_type == GL_UNSIGNED_SHORT ? 2 : 4;
        const std::uintptr_t start = m_index_offset + std::uintptr_t{first_index} * index_size;
        m_gl.DrawElementsInstanced(m_mode, static_cast<GLsizei>(index_count), m_index_type,
                                   reinterpret_cast<const void*>(start),
                                   static_cast<GLsizei>(instance_count));
    }

    // The interface's rectangles start at the top, and OpenGL's at the bottom, except in passes
    // into textures, which are drawn upside down.
    void set_scissor(std::optional<ScissorRect> rect) override {
        DREAM_VERIFY(m_in_pass, "Setting a scissor outside a render pass");
        const auto width = static_cast<std::int64_t>(m_pass_size.width);
        const auto height = static_cast<std::int64_t>(m_pass_size.height);
        std::int64_t left = 0;
        std::int64_t top = 0;
        std::int64_t right = width;
        std::int64_t bottom = height;
        if (rect.has_value()) {
            left = std::clamp<std::int64_t>(rect->x, 0, width);
            top = std::clamp<std::int64_t>(rect->y, 0, height);
            right = std::clamp<std::int64_t>(std::int64_t{rect->x} + rect->width, left, width);
            bottom = std::clamp<std::int64_t>(std::int64_t{rect->y} + rect->height, top, height);
        }
        const std::int64_t y = m_pass_into_textures ? top : height - bottom;
        m_gl.Scissor(static_cast<GLint>(left), static_cast<GLint>(y),
                     static_cast<GLsizei>(right - left), static_cast<GLsizei>(bottom - top));
    }

    Context m_context;
    Functions m_gl;
    bool m_ready = false;
    int m_version = 0;
    bool m_has_bc7 = false;
    bool m_has_astc = false;
    bool m_has_timer_queries = false;
    bool m_has_debug = false;
    bool m_has_float_targets = false;

    GLuint m_vertex_array = 0;
    std::array<GLuint, k_texture_slots> m_samplers{};
    GLuint m_uniform_ring = 0;
    std::size_t m_uniform_alignment = 256;
    std::size_t m_uniform_offset = 0;
    GLuint m_frame_buffer = 0;

    // The offscreen target, for windows that don't encode sRGB themselves.
    bool m_probed_window = false;
    bool m_window_encodes_srgb = true;
    GLuint m_offscreen_framebuffer = 0;
    GLuint m_offscreen_color = 0;
    GLuint m_offscreen_depth = 0;
    platform::SurfaceSize m_offscreen_size;
    GLuint m_copy_program = 0;
    TextureResource m_white_texture;
    TextureResource m_far_depth_texture;
    GLuint m_empty_fragment_shader = 0;
    std::vector<TargetFramebuffer> m_target_framebuffers;

    std::optional<CapturedFrame> m_captured;
    bool m_capture_requested = false;
    platform::SurfaceSize m_extent;
    platform::SurfaceSize m_requested_size;
    bool m_was_attached = false;

    bool m_in_frame = false;
    bool m_in_pass = false;
    std::optional<core::Error> m_frame_error;
    std::size_t m_frame_bytes_used = 0;
    GLuint m_vertex_buffer = 0;
    std::size_t m_vertex_buffer_offset = 0;
    GLsizei m_bound_stride = 0;
    bool m_vertex_buffer_dirty = true;
    GLuint m_bound_program = 0;
    std::array<GLuint, k_texture_slots> m_bound_textures{};
    // Slots holding a full-float texture, which read it without the slot's sampler.
    std::array<bool, k_texture_slots> m_unfiltered_slots{};
    platform::SurfaceSize m_pass_size;
    bool m_pass_into_textures = false;
    std::optional<TextureFormat> m_pass_color;
    std::optional<TextureFormat> m_pass_depth;
    std::array<GLuint, 2> m_pass_textures{};
    GLenum m_mode = GL_TRIANGLES;
    GLenum m_index_type = GL_UNSIGNED_SHORT;
    std::size_t m_index_offset = 0;
    std::uint32_t m_enabled_attributes = 0;
    GLsizei m_bound_instance_stride = 0;
    std::size_t m_instance_offset = 0;
    bool m_depth_write = true;

    core::HandlePool<TextureResource> m_textures;
    core::HandlePool<BufferResource> m_buffers;
    core::HandlePool<ShaderResource> m_shaders;
    core::HandlePool<PipelineResource> m_pipelines;

    // Enough for the frames a driver queues, and the one being recorded.
    std::array<TimingQuery, 4> m_timing_queries;
    bool m_timing_this_frame = false;
    std::size_t m_timing_oldest = 0;
    std::size_t m_timing_pending = 0;
    std::deque<double> m_gpu_frame_seconds;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& config) {
    auto device = std::make_unique<GlDevice>();
    if (core::Result<void> result = device->initialize(config); !result.has_value()) {
        return result.error();
    }
    return std::unique_ptr<Device>{std::move(device)};
}

} // namespace

#if DREAM_GL_ES
[[nodiscard]] core::Result<void> register_rhi_opengles(core::ModuleRegistry& registry) {
#else
[[nodiscard]] core::Result<void> register_rhi_opengl(core::ModuleRegistry& registry) {
#endif
    return registry.add(core::ModuleDescriptor<Device>{.name = k_name, .create = &create});
}

} // namespace dream::modules
