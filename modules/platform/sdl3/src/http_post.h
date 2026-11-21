#pragma once

#include "dream/core/result.h"
#include "dream/platform/platform.h"

namespace dream::modules::sdl3 {

// Sends an HTTP POST request and waits for the answer, with the system's own HTTP client.
[[nodiscard]] core::Result<platform::HttpResponse> post_http(const platform::HttpRequest& request);

} // namespace dream::modules::sdl3
