#include "dream/core/version.h"

namespace dream::core {

Version engine_version() noexcept {
    return Version{
        .major_number = DREAM_VERSION_MAJOR,
        .minor_number = DREAM_VERSION_MINOR,
        .patch_number = DREAM_VERSION_PATCH,
    };
}

} // namespace dream::core
