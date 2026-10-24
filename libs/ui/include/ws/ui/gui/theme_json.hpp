#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/json.hpp>
#include <ws/settings/migration.hpp>
#include <ws/ui/gui/theme_data.hpp>

namespace ws::ui::gui {

// The theme document's shape version - the same contract a settings
// layer file carries: write_theme stamps it, read_theme walks a
// document up to it through the caller's Migrations chain, and a file
// declaring a newer shape is refused, never guessed at. Version 1 is
// the stage-10 shape: accent, chrome (the faces then the metrics, the
// struct's own order), fonts, icon_set, shadows.
inline constexpr std::int64_t kThemeShapeVersion = 1;

// The theme facility's error codes (root CLAUDE.md, "Error handling"):
// every failure the text API returns is a ws::core::Error with
// category "ui" and one of these as its code - the inner settings
// error's reason rides the message, its category does not cross the
// ui boundary. Append-only: never renumber, only add.
enum class ThemeError : std::int32_t {
    parse_failed = 1,    // the text is not json
    migrate_failed = 2,  // the version walk refused (a newer shape, a gap)
    io_failed = 3,       // a read or write through the FileSystem seam failed
    bad_value = 4,       // a known key with the wrong type, or a color that
                         // is not #RRGGBBAA
};

// One-line description per code, from a static table (the jobs shape).
[[nodiscard]] constexpr std::string_view describe(ThemeError code) noexcept {
    switch (code) {
        case ThemeError::parse_failed: return "parse failed";
        case ThemeError::migrate_failed: return "migration refused";
        case ThemeError::io_failed: return "file access failed";
        case ThemeError::bad_value: return "bad theme value";
    }
    return "unknown theme error";
}

[[nodiscard]] inline ws::core::Error make_theme_error(ThemeError code,
                                                     std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "ui",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T> (the jobs shape):
//   return fail(ThemeError::bad_value, "chrome.text: expected a color");
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    ThemeError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_theme_error(code, std::move(message)));
}

// --- The text API ----------------------------------------------------------

// Parse + migrate + fold one theme document. `base` supplies the value
// of every member the text leaves out - the caller's starting point
// (default_dark()/default_light() for the stock documents) - so a
// partial file is a legal hand edit. The fold only folds:
// apply_accent is never re-run on the way in. The accent is remembered
// data like any face; a file that changes it without new faces keeps
// the faces it wrote, and an app that wants the derived set calls
// apply_accent(theme, theme.accent) itself after the load.
// Unknown TOP-LEVEL members are ignored here - read_theme owes
// nothing back; ThemeFile is the layer that remembers them for the
// next save. Stray keys nested inside a known group are dropped on the
// floor: only the document's own top level is round-tripped, the same
// line settings draws. The chrome's `font` pointer is runtime state
// and never crosses the text - the caller re-installs the loaded
// object.
[[nodiscard]] ws::core::Result<ThemeData> read_theme(
    std::string_view text, const ThemeData& base,
    const ws::settings::Migrations& migrations = {});

// The full document, every known member, canonical order: version,
// accent, chrome (faces then metrics), fonts, icon_set, shadows.
// Colors always write as 8-digit uppercase "#RRGGBBAA"; metrics
// always as reals - so a write -> read -> write cycle is
// byte-stable, and float fields survive it exactly (float -> double
// is lossless, the writer's shortest form re-parses to the same
// double, and the double casts back to the same float).
[[nodiscard]] std::string write_theme(const ThemeData& theme);

// --- The file layer --------------------------------------------------------

// One load() call's report. The caller uses `theme` in every case:
// the folded document on success, the base when the file is missing
// (a first run, not a failure) or unusable (the reason rides
// `error`).
struct ThemeLoad {
    bool missing = false;  // the file wasn't there: a first run
    bool failed = false;   // the file was there but couldn't be adopted
    std::string error;     // the reason, when failed
    ThemeData theme;       // what the caller should draw with

    [[nodiscard]] bool ok() const noexcept { return !failed; }
};

// One poll() call's report: the file's bytes changed since they were
// last seen, and either the new document was adopted (reloaded) or
// refused (failed - the last good theme stays live and the reason
// rides `error`).
struct ThemePoll {
    bool reloaded = false;
    bool failed = false;
    std::string error;
};

// One theme file: load, save, and the hot-reload poller - the dev-loop
// shape: edit the json while the app runs, the theme follows.
//
// The poller compares BYTES on a clock cadence (never mtime - editors
// write the same instant they touch, and bytes are the truth), gated
// by an injectable Clock answering steady milliseconds, the jobs
// Debouncer's shape, so tests drive it deterministically. A changed
// document that fails to parse or fold is reported once and kept OUT:
// the bad bytes are remembered, the last good theme stays live, no
// .bak is taken (the developer owns this file and the editor keeps
// its own undo), and the poll stays quiet until the file changes
// again. A file deleted after a good load reports once and keeps the
// loaded theme; a file that was never there polls silent - nothing to
// watch.
//
// save() writes the live theme plus the unknown top-level members the
// loaded file carried, in their file order, atomically through the
// FileSystem seam - and remembers the written bytes, so the next poll
// never reads the app's own save as a reload. The owner-thread rule
// is the settings one: one thread, one writer - a save while an
// editor holds unsaved changes to the same file clobbers them, by
// contract.
class ThemeFile {
public:
    // Steady milliseconds, the Debouncer's clock contract.
    using Clock = std::function<std::int64_t()>;

    // `base` is the fallback document (missing members, missing file,
    // unusable file). `poll_ms` clamps at 0; a null `clock` falls back
    // to the real steady clock.
    ThemeFile(std::string path, ws::settings::FileSystem& fs,
              const ws::settings::Migrations& migrations, ThemeData base,
              std::int64_t poll_ms = 250, Clock clock = {});

    ThemeFile(const ThemeFile&) = delete;
    ThemeFile& operator=(const ThemeFile&) = delete;

    // Read + parse + migrate + fold, once (startup, or a re-load after
    // the app knows better). A missing file answers missing - the
    // first run is not a failure. A failed load keeps the base and
    // remembers the bad bytes, so the poller stays quiet until the
    // file changes. The poll cadence starts here: the first poll()
    // waits a full poll_ms after this call.
    ThemeLoad load();

    // Write the live theme + the remembered unknowns, atomically.
    ws::core::Status save();

    // The hot-reload check - call once per frame. Answers at most one
    // transition per poll_ms. Before the first load() it stays silent.
    ThemePoll poll();

    // The live theme: what load() or poll() last adopted - `base`
    // until one of them does.
    [[nodiscard]] const ThemeData& theme() const noexcept { return theme_; }

    // The editor side (the gallery's accent picker, the theme page):
    // adopt a new document for the next save(). The file on disk is
    // not touched until save(), and poll() between the two stays
    // quiet - the live theme is not file state.
    void set_theme(ThemeData theme);

private:
    std::string path_;
    ws::settings::FileSystem& fs_;
    const ws::settings::Migrations& migrations_;
    ThemeData base_;
    ThemeData theme_;
    std::int64_t poll_ms_;
    Clock clock_;
    // INT64_MIN / 2 so the first poll always passes the cadence gate.
    std::int64_t last_poll_ = std::numeric_limits<std::int64_t>::min() / 2;
    bool load_ran_ = false;
    std::string bytes_;  // the bytes last seen: loaded, saved or refused
    // The remembered unknown members, in file order.
    std::vector<ws::settings::json::Object::Entry> unknowns_;
};

}  // namespace ws::ui::gui
