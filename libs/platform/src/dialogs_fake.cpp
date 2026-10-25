// FakeFileDialogs: the scripted stand-in the suite and the gallery
// drive - no OS, no blocking, just the queue in front and the
// recorded options behind. UnavailableFileDialogs lives inline in
// the header; this TU is the fake's half.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ws/platform/dialogs.hpp>

namespace ws::platform {
namespace {

// The front of a scripted queue, gone from the script once answered.
// An empty queue answers a dismissed dialog: nullopt for the single
// kinds, an empty vector for open_files.
template <typename T>
[[nodiscard]] T pop_front(std::vector<T>& queue) {
    if (queue.empty()) {
        return T{};
    }
    T out = std::move(queue.front());
    queue.erase(queue.begin());
    return out;
}

}  // namespace

ws::core::Result<std::optional<std::string>> FakeFileDialogs::open_file(
    const FileDialogOptions& options) {
    open_calls.push_back(options);
    return pop_front(open_queue);
}

ws::core::Result<std::vector<std::string>> FakeFileDialogs::open_files(
    const FileDialogOptions& options) {
    files_calls.push_back(options);
    return pop_front(files_queue);
}

ws::core::Result<std::optional<std::string>> FakeFileDialogs::save_file(
    const FileDialogOptions& options) {
    save_calls.push_back(options);
    return pop_front(save_queue);
}

ws::core::Result<std::optional<std::string>> FakeFileDialogs::select_folder(
    const FileDialogOptions& options) {
    folder_calls.push_back(options);
    return pop_front(folder_queue);
}

}  // namespace ws::platform
