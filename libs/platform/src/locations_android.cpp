// The Android half of os_location: there is none to compute. The app's
// directories live inside the activity's package space, which only the
// native glue knows - the app TU installs a LocationRoots at startup
// (the header's contract). Until then every location answers
// `unavailable`, never a guess.

#include <string>
#include <string_view>

#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>

namespace ws::platform {

ws::core::Result<std::string> os_location(Location which,
                                          std::string_view app_name) {
    (void)which;
    (void)app_name;
    return fail(PlatformError::unavailable,
                "android locations come from the activity's paths - "
                "install LocationRoots from the app TU");
}

}  // namespace ws::platform
