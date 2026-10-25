// The Windows half of the native pickers: the IFileDialog family
// (the Vista+ COM shell dialogs) behind FileDialogs. One TU, one
// edge - utf-8 narrow at the API, wide inside, converted once
// through the shared src-local win_utf8.hpp. Every call blocks
// while the user decides,
// on the caller's worker thread; the COM apartment is paid per
// call, never forced on the thread that built the set.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/platform/dialogs.hpp>
#include <ws/platform/errors.hpp>
#include <ws/platform/paths.hpp>

// windows.h last through the shared edge (win_utf8.hpp); the COM
// dialogs want objbase/shobjidl after it.
#include "win_utf8.hpp"
#include <objbase.h>
#include <shobjidl.h>

namespace ws::platform {
namespace {

[[nodiscard]] ws::core::unexpected<ws::core::Error> com_fail(
    std::string_view what) {
    return fail(PlatformError::io_failed,
                "file dialogs: " + std::string{what});
}

// The minimal owning COM pointer: this TU hands no raw Release.
template <typename T>
class ComRef {
  public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() { reset(); }

    // Fills a CoCreateInstance/QueryInterface out-param; resets first
    // so a reused ref never leaks the previous hold. One call site per
    // use - never inside a macro that could evaluate it twice.
    [[nodiscard]] T** put() {
        reset();
        return &ptr_;
    }
    void attach(T* taken) {
        reset();
        ptr_ = taken;
    }
    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

  private:
    T* ptr_ = nullptr;
};

// RAII per-call apartment. A lib that owns no threads pays COM init
// per dialog - a human-speed call - and never forces an apartment on
// the caller's thread. S_FALSE counts as initialized; its uninit
// pairs with it. RPC_E_CHANGED_MODE (the thread already runs the
// multithreaded apartment) is a refusal: a dialog cannot host there.
class ComApartment {
  public:
    ComApartment()
        : hr_{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)} {}
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_); }

  private:
    const HRESULT hr_;
};

// The one common step of every collect: an item's file-system path.
[[nodiscard]] ws::core::Result<std::string> path_of(IShellItem* item) {
    PWSTR raw = nullptr;
    if (item == nullptr ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) ||
        raw == nullptr) {
        return com_fail("the chosen item has no file-system path");
    }
    std::string out = to_utf8(raw);
    CoTaskMemFree(raw);
    return out;
}

// The start-in folder, from a parsing name. A path the shell cannot
// parse is skipped - the dialog falls back to its remembered folder
// (a wrong default_path is a hint, never a failure).
void set_default_folder(IFileDialog& dialog, std::string_view path) {
    if (path.empty()) {
        return;
    }
    const std::wstring wide = from_utf8(path);
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(
            wide.c_str(), nullptr, IID_IShellItem,
            reinterpret_cast<void**>(&folder))) &&
        folder != nullptr) {
        dialog.SetDefaultFolder(folder);
        folder->Release();
    }
}

// The type filters, verbatim: the OS is the matcher. The wide copies
// stay alive while the dialog holds the specs; half-formed entries
// (an empty name or pattern) are dropped, not fatal.
void set_filters(IFileDialog& dialog,
                 const std::vector<FileFilter>& filters) {
    if (filters.empty()) {
        return;
    }
    std::vector<std::wstring> names;
    std::vector<std::wstring> patterns;
    names.reserve(filters.size());
    patterns.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        if (filter.name.empty() || filter.pattern.empty()) {
            continue;
        }
        names.push_back(from_utf8(filter.name));
        patterns.push_back(from_utf8(filter.pattern));
    }
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        specs.push_back({names[i].c_str(), patterns[i].c_str()});
    }
    if (!specs.empty()) {
        dialog.SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }
}

// What a shown dialog answered: nullopt = dismissed, else the picked
// paths - one for the single kinds, any count for open_files.
using Picks = std::optional<std::vector<std::string>>;

// The one body every kind runs: create, configure, show, collect.
[[nodiscard]] ws::core::Result<Picks> show(const FileDialogOptions& options,
                                          bool pick_folders, bool multiple,
                                          bool save) {
    ComApartment com;
    if (!com.ok()) {
        return fail(PlatformError::unavailable,
                    "file dialogs: the thread's COM apartment cannot "
                    "host a dialog");
    }

    ComRef<IFileDialog> dialog;
    const CLSID& clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog,
                                reinterpret_cast<void**>(dialog.put())))) {
        return com_fail("CoCreateInstance failed");
    }

    DWORD flags = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (save) {
        flags |= FOS_OVERWRITEPROMPT;
    }
    if (pick_folders) {
        flags |= FOS_PICKFOLDERS;
    }
    if (multiple) {
        flags |= FOS_ALLOWMULTISELECT;
    }
    if (FAILED(dialog->SetOptions(flags))) {
        return com_fail("SetOptions failed");
    }

    // Title and default placement are hints: a failure to set either
    // never blocks the dialog - the OS falls back to its defaults.
    const std::wstring title = from_utf8(options.title);
    if (!title.empty()) {
        dialog->SetTitle(title.c_str());
    }
    if (!options.default_path.empty()) {
        if (save) {
            // the save dialog: the last segment seeds the file-name
            // box, the parent the starting folder
            const std::wstring file_name =
                from_utf8(name_of(options.default_path));
            if (!file_name.empty()) {
                dialog->SetFileName(file_name.c_str());
            }
            set_default_folder(*dialog.get(),
                              parent_of(options.default_path));
        } else {
            // the open and folder dialogs: the given folder to start in
            set_default_folder(*dialog.get(), options.default_path);
        }
    }
    if (!pick_folders) {
        set_filters(*dialog.get(), options.filters);
    }

    const HRESULT shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return Picks{};  // the dismissal: a normal answer, never an error
    }
    if (FAILED(shown)) {
        return com_fail("Show failed");
    }

    if (multiple) {
        // Only the open dialog's richer face collects many at once.
        ComRef<IFileOpenDialog> open_dialog;
        if (FAILED(dialog->QueryInterface(
                IID_IFileOpenDialog,
                reinterpret_cast<void**>(open_dialog.put()))) ||
            !open_dialog.valid()) {
            return com_fail("no multi-selection face");
        }
        ComRef<IShellItemArray> items;
        if (FAILED(open_dialog->GetResults(items.put())) ||
            !items.valid()) {
            return com_fail("GetResults failed");
        }
        DWORD count = 0;
        if (FAILED(items->GetCount(&count))) {
            return com_fail("GetCount failed");
        }
        std::vector<std::string> paths;
        paths.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            ComRef<IShellItem> item;
            if (FAILED(items->GetItemAt(i, item.put())) ||
                !item.valid()) {
                continue;  // what cannot be named is skipped, not fatal
            }
            WS_TRY_ASSIGN(const std::string path, path_of(item.get()));
            paths.push_back(path);
        }
        return paths;
    }

    ComRef<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put())) || !item.valid()) {
        return com_fail("GetResult failed");
    }
    WS_TRY_ASSIGN(const std::string path, path_of(item.get()));
    return std::vector<std::string>{path};
}

class WinFileDialogs final : public FileDialogs {
  public:
    [[nodiscard]] ws::core::Result<std::optional<std::string>> open_file(
        const FileDialogOptions& options) override {
        WS_TRY_ASSIGN(const Picks picks, show(options, false, false, false));
        return single_of(picks);
    }
    [[nodiscard]] ws::core::Result<std::vector<std::string>> open_files(
        const FileDialogOptions& options) override {
        WS_TRY_ASSIGN(const Picks picks, show(options, false, true, false));
        return picks.value_or(std::vector<std::string>{});
    }
    [[nodiscard]] ws::core::Result<std::optional<std::string>> save_file(
        const FileDialogOptions& options) override {
        WS_TRY_ASSIGN(const Picks picks, show(options, false, false, true));
        return single_of(picks);
    }
    [[nodiscard]] ws::core::Result<std::optional<std::string>> select_folder(
        const FileDialogOptions& options) override {
        WS_TRY_ASSIGN(const Picks picks, show(options, true, false, false));
        return single_of(picks);
    }

  private:
    // One path or a dismissal; a count other than one cannot happen
    // through GetResult, but is answered, not assumed away.
    [[nodiscard]] static ws::core::Result<std::optional<std::string>>
    single_of(const Picks& picks) {
        if (!picks || picks->empty()) {
            return std::optional<std::string>{};
        }
        return (*picks)[0];
    }
};

}  // namespace

std::unique_ptr<FileDialogs> make_file_dialogs() {
    return std::make_unique<WinFileDialogs>();
}

}  // namespace ws::platform
