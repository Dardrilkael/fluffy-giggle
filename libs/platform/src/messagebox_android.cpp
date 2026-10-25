// The Android refusal: the activity owns its dialogs (an
// AlertDialog through the JNI), which only the app TU knows - the
// clipboard/locations precedent.

#include <memory>

#include <ws/platform/messagebox.hpp>

namespace ws::platform {

std::unique_ptr<MessageBoxes> make_message_boxes() {
    return std::make_unique<UnavailableMessageBoxes>();
}

}  // namespace ws::platform
