#pragma once

// ws::platform - the Storage Access Framework bridge's pure half
// (lifted from apps/circsim_editor, stage 11 - the lib extraction):
// everything about a SAF-backed document store that touches no Android
// - the sandbox-name policy, the persisted path->URI map, and the
// write-through FileSystem wrapper - so the whole data policy is
// headless-testable (libs/platform/tests/test_saf.cpp) and any app's
// JNI half stays thin (circsim_editor's android_saf.cpp pairs with its
// CircsimActivity Java shim and stays app-side).
//
// The port's shape: a picked SAF document (a content:// URI the shell's
// path-shaped world cannot read) becomes a REAL FILE under the app's
// private sandbox - <files>/circsim editor/schematics/<name>-<hash>.csim
// - and the map remembers which URI the file stands for. Every path the
// shell, the documents and the session see is a plain filesystem path,
// so export-beside-document, recents, autosave and session restore work
// unchanged; the wrapper pushes each write through to the URI after it
// lands locally, so the user's picked location never goes stale (a
// refused push degrades to "saved locally" with the error recorded, not
// a lost save).

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::platform::saf {

// One persisted sandbox-file -> content-URI pair.
struct SafEntry {
    std::string path;  // the sandbox file, '/'-canonical
    std::string uri;   // the SAF document it mirrors, "content://..."
};

// The map's whole surface: a tiny ordered vector (a phone edits a
// handful of schematics), looked up by path. Load/parse answers the
// entries that parse; a bad line drops (a hand-edited or truncated map
// degrades, never kills).
class SafMap {
  public:
    // Parses "path\turi" lines (the format_map spelling). Empty and
    // ill-formed lines drop; a duplicate path keeps the LAST entry (the
    // newest pick wins - exactly what add() would have done).
    void load(std::string_view bytes);

    // The map as "path\turi\n" lines, load()'s mirror image.
    [[nodiscard]] std::string format() const;

    void add(std::string path, std::string uri);
    void remove(std::string_view path);

    // The URI a sandbox path mirrors, or null.
    [[nodiscard]] const std::string* find(std::string_view path) const;

    [[nodiscard]] const std::vector<SafEntry>& entries() const {
        return entries_;
    }

  private:
    std::vector<SafEntry> entries_;
};

// FNV-1a 64 over the bytes, spelled as 8 lowercase hex digits - the
// disambiguator two different providers' same-named documents need.
[[nodiscard]] std::string hash_tag(std::string_view bytes);

// The sandbox file name for a picked document: the picker's display
// name, sanitized (separators and control bytes folded away, whitespace
// trimmed, the length capped), with "-<hash8>" spliced before the
// extension so two same-named documents from different folders stay two
// files (and the same URI stays ONE file across launches - the stable
// identity session restore and the recent list ride). A display name
// that sanitizes away to nothing falls back to `fallback`; an extension
// survives, a missing one gets `default_ext` (the app's document
// extension - ".csim" for the schematic editor; the policy is the
// caller's, the lib stays document-agnostic).
[[nodiscard]] std::string sandbox_name_for(std::string_view display,
                                           std::string_view uri,
                                           std::string_view fallback,
                                           std::string_view default_ext);

// One push of a saved document's bytes to its content URI. The JNI half
// injects the ContentResolver implementation; the headless tests inject
// a recorder. A push that fails records the error (the local save
// stands - the honest degrade), it never fails the write.
using SafPush = std::function<ws::core::Status(const std::string& uri,
                                               std::string_view bytes)>;

// The write-through FileSystem the app hands the shell in the SAF
// build: every verb delegates to `base` (the real, local answer), and a
// write to a MAPPED sandbox path additionally pushes the same bytes to
// the document's URI. `map_path` is the file the map persists to,
// THROUGH THIS SAME SEAM (an unmapped path: no recursion, no push).
//
// The push error policy: the local write already succeeded when the
// push runs, so a refused push is recorded (last_push_error_, drained
// through take_push_error - the app toasts it between frames) and the
// write answers ok. A save is never reported failed over bytes that
// are, in fact, safely on disk.
//
// Threading: the map and the error slot sit under one mutex because
// two threads reach them - the main thread's saves (the shell's
// landed completions, the autosave) and the picker worker's map() as
// picks land. The base seam's own verbs stay unguarded: StdFileSystem
// is stateless, and the paths the two threads write never coincide
class WriteThroughFs final : public ws::settings::FileSystem {
  public:
    WriteThroughFs(ws::settings::FileSystem& base, SafMap& map,
                   SafPush push, std::string map_path);

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override;
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override;
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override;
    bool remove(std::string_view path) override;
    [[nodiscard]] bool exists(std::string_view path) const override;

    // Maps a sandbox path to its URI and persists the map. The SAF
    // dialogs call this as picks land.
    void map(std::string path, std::string uri);

    // The last refused push, if any - drained by the reader (the app's
    // between-frames toast poll).
    [[nodiscard]] std::optional<std::string> take_push_error();

  private:
    void save_map();

    ws::settings::FileSystem& base_;
    SafMap& map_;
    SafPush push_;
    std::string map_path_;
    std::mutex mutex_;  // map_ + push_error_ (the two-thread rule above)
    std::optional<std::string> push_error_;
};

}  // namespace ws::platform::saf
