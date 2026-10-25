// The Windows half of the message boxes: MessageBoxW, one call,
// no parent window - the OS owns the box, and ui's window is not
// platform's to parent on. Blocks until answered, like the
// dialogs: the caller's worker thread.

#include <memory>
#include <string>

#include <ws/platform/messagebox.hpp>
#include <ws/platform/errors.hpp>

#include "win_utf8.hpp"

namespace ws::platform {
namespace {

[[nodiscard]] UINT flags_of(const MessageBoxRequest& request) {
    UINT flags = 0;
    switch (request.kind) {
        case MessageBoxKind::info:
            flags |= MB_ICONINFORMATION;
            break;
        case MessageBoxKind::warning:
            flags |= MB_ICONWARNING;
            break;
        case MessageBoxKind::error:
            flags |= MB_ICONERROR;
            break;
    }
    switch (request.buttons) {
        case MessageBoxButtons::ok:
            flags |= MB_OK;
            break;
        case MessageBoxButtons::ok_cancel:
            flags |= MB_OKCANCEL;
            break;
        case MessageBoxButtons::yes_no:
            flags |= MB_YESNO;
            break;
    }
    return flags;
}

class WinMessageBoxes final : public MessageBoxes {
  public:
    [[nodiscard]] ws::core::Result<MessageBoxChoice> ask(
        const MessageBoxRequest& request) override {
        const std::wstring title = from_utf8(request.title);
        const std::wstring message = from_utf8(request.message);
        const int answer = MessageBoxW(nullptr, message.c_str(),
                                       title.c_str(),
                                       flags_of(request));
        switch (answer) {
            case IDOK:
                return MessageBoxChoice::ok;
            case IDCANCEL:
                return MessageBoxChoice::cancel;
            case IDYES:
                return MessageBoxChoice::yes;
            case IDNO:
                return MessageBoxChoice::no;
            default:
                break;
        }
        return fail(PlatformError::io_failed,
                    "message boxes: showing the box failed");
    }
};

}  // namespace

std::unique_ptr<MessageBoxes> make_message_boxes() {
    return std::make_unique<WinMessageBoxes>();
}

}  // namespace ws::platform
