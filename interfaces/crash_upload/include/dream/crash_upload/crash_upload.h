#pragma once

#include "dream/core/result.h"
#include "dream/platform/platform.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::crash_upload {

// Uploads crash reports to a service where a game's makers can see them (ADR 0043). Core writes a
// report, and on Windows a minidump, when a game crashes; the next time the game starts, the
// runtime hands each saved report to the project's crash_upload module, which turns it into the
// requests its service takes. The runtime sends them through the platform, so a module needs no
// network code of its own, and can be tested without a network.

// One crash, as Core saved it.
struct Report {
    // The report's file name, without its extension, such as "crash-20261120-101500-4242".
    std::string name;
    // The text report: reason, address, frame, modules, memory, the last log lines and the stack.
    std::string text;
    // Windows: the minidump, with every thread's stack. Empty where there is none, as on Android.
    std::vector<std::byte> minidump;
    // The game's title, its platform, such as "windows" or "android", and the engine's version.
    std::string application;
    std::string platform;
    std::string engine_version;
    // The game's version when it crashed, as its report says, such as "1.4.2", or empty (ADR 0048).
    // Services match it with the symbols uploaded for that release.
    std::string application_version;
};

// Sends one request, and gives the answer. Fails when the network does.
using Sender = std::function<core::Result<platform::HttpResponse>(const platform::HttpRequest&)>;

class CrashUploader {
public:
    static constexpr std::string_view k_module_interface = "crash_upload";

    virtual ~CrashUploader() = default;

    // Whether this module sends reports anywhere. One that doesn't, such as `null`, leaves them
    // where they are.
    [[nodiscard]] virtual bool sends() const = 0;

    // Sends one report through `send`. Fails if the network does, or the service refuses it; the
    // report is then kept, and tried again the next time the game starts.
    [[nodiscard]] virtual core::Result<void> upload(const Report& report, const Sender& send) = 0;
};

} // namespace dream::crash_upload
