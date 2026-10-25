// The native ask behind one interface, so tests and the gallery
// can script a fake or accept the refusal - the dialogs pattern,
// one method. A box cannot be dismissed without a choice (the X
// maps to cancel on ok_cancel, to the only choice on ok), so
// ask() always answers; the choice is the answer, never nullopt.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/platform/errors.hpp>

namespace ws::platform {

// The kinds map to the OS's icons; the button sets to its rows.
enum class MessageBoxKind { info, warning, error };
enum class MessageBoxButtons { ok, ok_cancel, yes_no };
enum class MessageBoxChoice { ok, cancel, yes, no };

struct MessageBoxRequest {
    MessageBoxKind kind = MessageBoxKind::info;
    MessageBoxButtons buttons = MessageBoxButtons::ok;
    std::string title;
    std::string message;
};

// A native ask BLOCKS until answered: the caller's worker thread,
// never the render loop.
class MessageBoxes {
  public:
    virtual ~MessageBoxes() = default;
    [[nodiscard]] virtual ws::core::Result<MessageBoxChoice> ask(
        const MessageBoxRequest& request) = 0;
};

// The android answer (the activity owns its dialogs, reached only
// through the JNI the app TU knows) and a public class so the
// refusal is testable everywhere.
class UnavailableMessageBoxes final : public MessageBoxes {
  public:
    [[nodiscard]] ws::core::Result<MessageBoxChoice> ask(
        const MessageBoxRequest& /*request*/) override {
        return fail(PlatformError::unavailable,
                    "message boxes: unavailable on this platform");
    }
};

// Scripts the answers and records every request. An empty queue
// answers the request's first choice - the click-through - so an
// unscripted ask never invents a choice the buttons did not
// offer. The crash-recovery demo's ask rides this fake.
class FakeMessageBoxes final : public MessageBoxes {
  public:
    std::vector<MessageBoxChoice> queue;   // popped front first
    std::vector<MessageBoxRequest> calls;  // every ask, verbatim

    [[nodiscard]] ws::core::Result<MessageBoxChoice> ask(
        const MessageBoxRequest& request) override;
};

// The platform's native set: MessageBoxW on windows, the refusal
// on android.
[[nodiscard]] std::unique_ptr<MessageBoxes> make_message_boxes();

}  // namespace ws::platform
