// HTTPS POST requests for Platform::post_http (ADR 0043): WinHTTP on Windows, Java's
// HttpURLConnection through JNI on Android, and nothing elsewhere yet. Both come with the system, so
// the engine needs no HTTP library.

#include "http_post.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace dream::modules::sdl3 {
namespace {

core::Error http_error(std::string message) {
    return core::Error{core::ErrorCode::IoError, std::move(message)};
}

#if defined(_WIN32)

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

class Handle {
public:
    explicit Handle(HINTERNET handle) : m_handle(handle) {}
    ~Handle() {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HINTERNET get() const {
        return m_handle;
    }

private:
    HINTERNET m_handle;
};

core::Error last_error(std::string_view what) {
    return http_error(std::format("{}: Windows error {}", what, GetLastError()));
}

core::Result<platform::HttpResponse> post(const platform::HttpRequest& request) {
    std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        return last_error(std::format("'{}' isn't a URL", request.url));
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const Handle session(WinHttpOpen(L"DreamEngine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr) {
        return last_error("Opening WinHTTP");
    }
    const int timeout = static_cast<int>(request.timeout_seconds * 1000);
    WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
    const Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (connection.get() == nullptr) {
        return last_error(std::format("Connecting to '{}'", request.url));
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const Handle sent(WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (sent.get() == nullptr) {
        return last_error("Opening the request");
    }
    std::wstring headers;
    for (const auto& [name, value] : request.headers) {
        headers += widen(name) + L": " + widen(value) + L"\r\n";
    }
    void* const body = const_cast<std::byte*>(request.body.data());
    const auto size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(sent.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(-1), body, size, size, 0) ||
        !WinHttpReceiveResponse(sent.get(), nullptr)) {
        return last_error(std::format("Sending to '{}'", request.url));
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(sent.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    platform::HttpResponse response{.status = static_cast<int>(status), .body = {}};
    // The answer, up to 64 KB, which is all an upload's reply needs.
    while (response.body.size() < 65536) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(sent.get(), &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(sent.get(), chunk.data(), available, &read) || read == 0) {
            break;
        }
        response.body.append(chunk.data(), read);
    }
    return response;
}

#elif defined(__ANDROID__)

// Takes a pending Java exception, as an error naming what was being done.
bool failed(JNIEnv* env, std::string_view what, core::Error& error) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string text = "a Java exception";
    if (thrown != nullptr) {
        jclass object = env->FindClass("java/lang/Object");
        jmethodID to_string = env->GetMethodID(object, "toString", "()Ljava/lang/String;");
        auto* described = static_cast<jstring>(env->CallObjectMethod(thrown, to_string));
        if (!env->ExceptionCheck() && described != nullptr) {
            const char* chars = env->GetStringUTFChars(described, nullptr);
            text = chars;
            env->ReleaseStringUTFChars(described, chars);
        }
        env->ExceptionClear();
    }
    error = http_error(std::format("{}: {}", what, text));
    return true;
}

core::Result<platform::HttpResponse> post(const platform::HttpRequest& request) {
    // SDL attaches the calling thread to Java if it isn't already.
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
        return http_error("There is no Java environment for this thread");
    }
    if (env->PushLocalFrame(64) != 0) {
        return http_error("Out of Java local references");
    }
    core::Error error;
    const auto done = [&](core::Result<platform::HttpResponse> result) {
        env->PopLocalFrame(nullptr);
        return result;
    };

    jclass url_class = env->FindClass("java/net/URL");
    jmethodID url_init = env->GetMethodID(url_class, "<init>", "(Ljava/lang/String;)V");
    jobject url = env->NewObject(url_class, url_init, env->NewStringUTF(request.url.c_str()));
    if (failed(env, std::format("'{}' isn't a URL", request.url), error)) {
        return done(error);
    }
    jmethodID open = env->GetMethodID(url_class, "openConnection", "()Ljava/net/URLConnection;");
    jobject connection = env->CallObjectMethod(url, open);
    if (failed(env, "Opening the connection", error)) {
        return done(error);
    }
    jclass http = env->FindClass("java/net/HttpURLConnection");
    const auto call_void = [&](const char* name, const char* signature, auto... arguments) {
        jmethodID method = env->GetMethodID(http, name, signature);
        env->CallVoidMethod(connection, method, arguments...);
    };
    const jint timeout = static_cast<jint>(request.timeout_seconds * 1000);
    call_void("setRequestMethod", "(Ljava/lang/String;)V", env->NewStringUTF("POST"));
    call_void("setDoOutput", "(Z)V", JNI_TRUE);
    call_void("setConnectTimeout", "(I)V", timeout);
    call_void("setReadTimeout", "(I)V", timeout);
    call_void("setFixedLengthStreamingMode", "(I)V", static_cast<jint>(request.body.size()));
    for (const auto& [name, value] : request.headers) {
        call_void("setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
                  env->NewStringUTF(name.c_str()), env->NewStringUTF(value.c_str()));
    }
    if (failed(env, "Preparing the request", error)) {
        return done(error);
    }

    jmethodID get_output = env->GetMethodID(http, "getOutputStream", "()Ljava/io/OutputStream;");
    jobject output = env->CallObjectMethod(connection, get_output);
    if (failed(env, std::format("Connecting to '{}'", request.url), error)) {
        return done(error);
    }
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(request.body.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(request.body.size()),
                            reinterpret_cast<const jbyte*>(request.body.data()));
    jclass output_class = env->FindClass("java/io/OutputStream");
    env->CallVoidMethod(output, env->GetMethodID(output_class, "write", "([B)V"), bytes);
    env->CallVoidMethod(output, env->GetMethodID(output_class, "close", "()V"));
    if (failed(env, std::format("Sending to '{}'", request.url), error)) {
        return done(error);
    }

    platform::HttpResponse response;
    response.status =
        env->CallIntMethod(connection, env->GetMethodID(http, "getResponseCode", "()I"));
    if (failed(env, "Reading the answer", error)) {
        return done(error);
    }
    // The answer, up to 64 KB, from the error stream for a failure status.
    jobject input = env->CallObjectMethod(
        connection, env->GetMethodID(http, response.status >= 400 ? "getErrorStream"
                                                                  : "getInputStream",
                                     "()Ljava/io/InputStream;"));
    env->ExceptionClear();
    if (input != nullptr) {
        jclass input_class = env->FindClass("java/io/InputStream");
        jmethodID read = env->GetMethodID(input_class, "read", "([B)I");
        jbyteArray buffer = env->NewByteArray(4096);
        while (response.body.size() < 65536) {
            const jint count = env->CallIntMethod(input, read, buffer);
            if (env->ExceptionCheck() || count <= 0) {
                env->ExceptionClear();
                break;
            }
            std::string chunk(static_cast<std::size_t>(count), '\0');
            env->GetByteArrayRegion(buffer, 0, count, reinterpret_cast<jbyte*>(chunk.data()));
            response.body += chunk;
        }
        env->CallVoidMethod(input, env->GetMethodID(input_class, "close", "()V"));
        env->ExceptionClear();
    }
    call_void("disconnect", "()V");
    env->ExceptionClear();
    return done(response);
}

#else

core::Result<platform::HttpResponse> post(const platform::HttpRequest& /*request*/) {
    return http_error("HTTP requests aren't supported on this platform yet");
}

#endif

} // namespace

core::Result<platform::HttpResponse> post_http(const platform::HttpRequest& request) {
    return post(request);
}

} // namespace dream::modules::sdl3
