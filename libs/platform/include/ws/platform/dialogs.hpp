#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/platform/errors.hpp>

namespace ws::platform {

// One entry in a dialog's type filter: a human name ("Text files")
// and the OS-shaped pattern it stands for ("*.txt"), passed through
// verbatim - the OS is the matcher, never this lib.
struct FileFilter {
    std::string name;
    std::string pattern;
};

// What a dialog is asked with. An empty title leaves the OS default;
// an empty default_path leaves the OS's remembered folder. The
// default_path is the FOLDER to start in for the open and folder
// dialogs; for the save dialog it may name the file - its last
// segment seeds the file-name box, its parent the starting folder.
struct FileDialogOptions {
    std::string title;
    std::string default_path;
    std::vector<FileFilter> filters;
};

// The pickers behind one interface, so tests and the gallery script
// a fake (FakeFileDialogs below) or accept a refusal
// (UnavailableFileDialogs, the android answer - the Storage Access
// Framework lives behind the activity, which only the app TU knows).
// Every call BLOCKS while the user decides: run it on a worker
// (ws::jobs), never the render loop. A dismissed dialog is nullopt
// - an empty list from open_files - a normal answer, never an
// error; errors carry the platform failing (unavailable, the COM or
// shell failing on windows). Results are utf-8 narrow paths,
// '/'-canonical at the OS edge.
class FileDialogs {
  public:
    virtual ~FileDialogs() = default;

    [[nodiscard]] virtual ws::core::Result<std::optional<std::string>>
    open_file(const FileDialogOptions& options) = 0;
    // Multiple files at once; an empty vector is a dismissed dialog.
    [[nodiscard]] virtual ws::core::Result<std::vector<std::string>>
    open_files(const FileDialogOptions& options) = 0;
    [[nodiscard]] virtual ws::core::Result<std::optional<std::string>>
    save_file(const FileDialogOptions& options) = 0;
    [[nodiscard]] virtual ws::core::Result<std::optional<std::string>>
    select_folder(const FileDialogOptions& options) = 0;
};

// The platform that cannot answer: every kind refuses `unavailable`.
// A public class so the refusal is testable everywhere; the android
// factory hands this out until the app TU wires its SAF-backed set.
class UnavailableFileDialogs final : public FileDialogs {
  public:
    [[nodiscard]] ws::core::Result<std::optional<std::string>> open_file(
        const FileDialogOptions& /*options*/) override {
        return fail(PlatformError::unavailable,
                    "file dialogs: unavailable on this platform");
    }
    [[nodiscard]] ws::core::Result<std::vector<std::string>> open_files(
        const FileDialogOptions& /*options*/) override {
        return fail(PlatformError::unavailable,
                    "file dialogs: unavailable on this platform");
    }
    [[nodiscard]] ws::core::Result<std::optional<std::string>> save_file(
        const FileDialogOptions& /*options*/) override {
        return fail(PlatformError::unavailable,
                    "file dialogs: unavailable on this platform");
    }
    [[nodiscard]] ws::core::Result<std::optional<std::string>> select_folder(
        const FileDialogOptions& /*options*/) override {
        return fail(PlatformError::unavailable,
                    "file dialogs: unavailable on this platform");
    }
};

// The scripted fake: each call records its options and pops the front
// of its queue; an empty queue answers a dismissed dialog, and so
// does a nullopt (or empty vector) entry. Public state, the
// FileSystem fake's shape - the queues are the script, the *_calls
// the record for the gallery's round trip.
class FakeFileDialogs final : public FileDialogs {
  public:
    std::vector<std::optional<std::string>> open_queue;
    std::vector<std::vector<std::string>> files_queue;
    std::vector<std::optional<std::string>> save_queue;
    std::vector<std::optional<std::string>> folder_queue;

    std::vector<FileDialogOptions> open_calls;
    std::vector<FileDialogOptions> files_calls;
    std::vector<FileDialogOptions> save_calls;
    std::vector<FileDialogOptions> folder_calls;

    [[nodiscard]] ws::core::Result<std::optional<std::string>> open_file(
        const FileDialogOptions& options) override;
    [[nodiscard]] ws::core::Result<std::vector<std::string>> open_files(
        const FileDialogOptions& options) override;
    [[nodiscard]] ws::core::Result<std::optional<std::string>> save_file(
        const FileDialogOptions& options) override;
    [[nodiscard]] ws::core::Result<std::optional<std::string>> select_folder(
        const FileDialogOptions& options) override;
};

// The native dialog set for this platform: windows answers the COM
// pickers, android the refusal above. Constructing it is cheap -
// the COM apartment is paid per call, not per set.
[[nodiscard]] std::unique_ptr<FileDialogs> make_file_dialogs();

}  // namespace ws::platform
