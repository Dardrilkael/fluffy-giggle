// The Android half of the native pickers: there is none. The
// Storage Access Framework lives behind the activity's JNI, which
// only the native-app TU knows - until it installs its own
// SAF-backed FileDialogs, the factory hands out the refusal
// (UnavailableFileDialogs, the header's public class, so the
// refusal itself is testable everywhere).

#include <memory>

#include <ws/platform/dialogs.hpp>

namespace ws::platform {

std::unique_ptr<FileDialogs> make_file_dialogs() {
    return std::make_unique<UnavailableFileDialogs>();
}

}  // namespace ws::platform
