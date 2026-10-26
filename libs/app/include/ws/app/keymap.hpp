#pragma once

#include <string>
#include <string_view>

#include <ws/app/shortcut.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::app {

// The user keymap as a file: the whole live table - every context's
// bindings and sequences, no provenance (the resolver keeps none) -
// serialized through the ws::settings json + FileSystem seams, the
// recent-files precedent, never a duplicated writer. The app's flow:
//
//     ShortcutResolver resolver;
//     resolver.install_defaults(registry);
//     Keymap keymap{fs, settings_dir + "/keymap.json"};
//     keymap.load(resolver);      // first run: the seed stands
//     ... resolver.rebind(...)    // the editor
//     keymap.save(resolver);      // on quit / debounced
//
// The document's shape is versioned; shape 1 is
//
//     { "version": 1,
//       "contexts": {
//         "global":      { "bindings":   [ { "command": "...",
//                                            "chord": { "key": 83, "mods": 2 } } ],
//                            "sequences": [ { "command": "...",
//                                             "first":  { ... }, "second": { ... },
//                                             "timeout_ms": 1000 } ] },
//         "panel":       { ... },
//         "text_editing": { ... } } }
//
// Save always writes all three contexts (a byte-stable round trip:
// save -> load -> save re-emits the same bytes, because table order
// survives install). Load is all-or-nothing: the whole document
// validates before anything installs, and any refusal leaves the
// resolver exactly as it was - a missing file is the FIRST RUN and
// leaves the defaults seed standing. Unknown keys - a field not
// written today, a context this version does not know - are forward
// data from a newer file and are ignored, never a refusal. There is
// no .bak: the keymap is re-editable convenience data over a
// re-seedable defaults table (the recent-files policy), and a
// hand-edit gone wrong is one reset_to_defaults() away.
class Keymap {
public:
    // Caller owns both halves (the platform FileDialog/recent shape):
    // the seam outlives the keymap, the resolver outlives every call.
    Keymap(ws::settings::FileSystem& fs, std::string path);

    // Reads the file into the resolver (import_text's rules). A
    // MISSING file is the first run: success with the resolver
    // untouched - never wipe a seed because the file is not there
    // yet. A read failure on an existing file answers io_failed; a
    // parse failure parse_failed; a well-formed document of the wrong
    // shape corrupt_file. Nothing installs unless the whole document
    // validates.
    [[nodiscard]] ws::core::Status load(ShortcutResolver& resolver);

    // Writes the resolver's whole live table. The seam's write is
    // ATOMIC (sibling ".tmp" + rename): a failure anywhere answers
    // with the seam's own Status as-is and leaves the previous file
    // byte-identical.
    [[nodiscard]] ws::core::Status save(const ShortcutResolver& resolver);

    // The same document as text, for the keymap editor's copy field
    // and the import/export file-dialog paths.
    [[nodiscard]] std::string export_text(const ShortcutResolver& resolver) const;

    // import_text's parse/refusal rules, minus the file: corrupt or
    // clashing text answers with nothing installed.
    [[nodiscard]] ws::core::Status import_text(std::string_view text,
                                               ShortcutResolver& resolver);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    ws::settings::FileSystem& fs_;
    std::string path_;
};

}  // namespace ws::app
