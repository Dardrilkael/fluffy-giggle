// The scripted half of the message boxes: every ask recorded, the
// queue popped front-first. An empty queue answers the request's
// first choice - ok for the ok-shaped sets, yes for yes_no - so an
// unscripted ask never invents a choice the buttons did not offer.

#include <ws/platform/messagebox.hpp>

namespace ws::platform {

ws::core::Result<MessageBoxChoice> FakeMessageBoxes::ask(
    const MessageBoxRequest& request) {
    calls.push_back(request);
    if (queue.empty()) {
        return request.buttons == MessageBoxButtons::yes_no
                   ? MessageBoxChoice::yes
                   : MessageBoxChoice::ok;
    }
    const MessageBoxChoice choice = queue.front();
    queue.erase(queue.begin());
    return choice;
}

}  // namespace ws::platform
