#pragma once

// The document layer (PLAN_UI_STAGE14_15 stage 15): the Document
// interface the host implements and the DocumentManager that owns the
// open set. The minimal in-repo version of Part A's document story -
// the seam is clear so a future full Part A can replace it.
//
// Split of labor: a Document knows its own bytes (load/save/content),
// its own undo history and its own display name; the manager knows the
// SET (open, close, activate, session restore) and owns the autosave
// pair (ws::platform::Autosave) per document - crash recovery is a
// property of "an open document in this app", not of any one type's
// bytes. The dirty flag rides the undo stack's save point (the plan's
// "dirty flag via undo save-point"): is_dirty() IS the document's
// unsaved-work answer.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ws/app/undo.hpp>
#include <ws/core/result.hpp>
#include <ws/platform/autosave.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::ui::shell {

// The host-implemented unit of editable content. Owner-thread; the
// manager calls load()/save() on the caller's thread (never a render
// or audio callback - the workspace rule).
class Document {
public:
    virtual ~Document() = default;

    // "" = untitled (never saved). The manager's save-as flow calls
    // set_path() first, then save().
    [[nodiscard]] virtual const std::string& path() const = 0;
    virtual void set_path(std::string path) = 0;

    // The title-bar/menu name: "notes.txt" for a file, "untitled" for
    // a fresh one (the manager composes the window title around it).
    [[nodiscard]] virtual std::string display_name() const = 0;

    // The recent-files list key (ws::platform::RecentFiles' per-type
    // lists): "scratch", "scene", ... "" lands the app-default list.
    [[nodiscard]] virtual std::string doc_type() const = 0;

    // Reads path() into the document. A failure answers as-is (the
    // manager renders it and refuses the open).
    [[nodiscard]] virtual ws::core::Status load() = 0;

    // Writes the document to `path` (path() by default; save-as passes
    // the target AFTER set_path moved it). Implementations that keep
    // the undo history should mark the save point through undo().
    [[nodiscard]] virtual ws::core::Status save() = 0;

    // The undo history - the manager's dirty tracking reads its
    // save-point comparison, and the shell's Undo/Redo commands drive
    // it. One stack per document (the plan's contract).
    [[nodiscard]] virtual ws::app::UndoStack& undo() = 0;

    // The job-cancel group this document's background work runs under
    // (0 = none). The shell forwards it to ws::jobs' cancel_group when
    // the document closes, so a closing document's jobs stop first.
    [[nodiscard]] virtual std::uint64_t job_group() const { return 0; }

    // The autosave pair's content seam: what snapshot() writes and
    // what a recovery hands back. Default: no content (no autosave).
    [[nodiscard]] virtual std::string autosave_content() const { return {}; }
    virtual void adopt_autosave_content(std::string_view) {}
};

// The factory the host provides: one document for one path ("" = a
// fresh untitled one). A factory that cannot produce (unknown
// extension, out of memory) answers an error; the manager renders it.
using DocumentFactory =
    std::function<ws::core::Result<std::unique_ptr<Document>>(
        const std::string& path)>;

// The open-document set. Owner-thread state, no locks; every callback
// (factory, recents hook) runs on the caller's thread.
class DocumentManager {
public:
    // `fs` is the autosave store's seam (it must outlive the manager);
    // `autosave_dir` the store directory ("" disables autosave - the
    // headless default, the tests' choice).
    DocumentManager(DocumentFactory factory, ws::settings::FileSystem* fs,
                    std::string autosave_dir);

    DocumentManager(const DocumentManager&) = delete;
    DocumentManager& operator=(const DocumentManager&) = delete;
    ~DocumentManager();

    // --- the open set -----------------------------------------------------

    // Creates through the factory, load()s, adopts and activates. A
    // load failure answers the error (nothing adopted); the same path
    // already open just activates it.
    [[nodiscard]] ws::core::Result<int> open_path(const std::string& path);

    // Adopts a document the host built by hand (a fresh "untitled" one
    // via the factory's "" path, or a recovery the host reconstructed)
    // WITHOUT loading it. Returns its index.
    [[nodiscard]] int adopt(std::unique_ptr<Document> document);

    // A fresh untitled document through the factory ("").
    [[nodiscard]] ws::core::Result<int> new_document();

    // What closing one document takes. `prompt` is true when the
    // document is dirty and the caller must ask save/discard/cancel
    // first; finish_close() runs the close.
    struct ClosePlan {
        bool prompt = false;
        std::size_t index = 0;
        bool valid = false;  // an in-range index existed
    };
    [[nodiscard]] ClosePlan plan_close(std::size_t index);

    // Closes: with save_first, save() runs first and its failure
    // answers with the document still open; without, the changes are
    // lost. Cancels the document's job group, fires the autosave
    // finish/discard, drops the document.
    [[nodiscard]] ws::core::Status finish_close(std::size_t index,
                                                bool save_first);

    [[nodiscard]] std::size_t count() const noexcept { return documents_.size(); }
    [[nodiscard]] Document* at(std::size_t index);
    [[nodiscard]] const Document* at(std::size_t index) const;
    [[nodiscard]] Document* active();
    [[nodiscard]] const Document* active() const;
    [[nodiscard]] int active_index() const noexcept { return active_; }

    void activate(std::size_t index);

    // Fired with the loaded path after a successful open_path (the
    // shell wires this to RecentFiles::touch).
    std::function<void(std::string_view, std::string_view)> on_opened_path;

    // --- the session -------------------------------------------------------

    // What startup restores: the open set's paths (untitled documents
    // carry "" and are skipped) and the active index.
    struct Session {
        std::vector<std::string> paths;
        int active = -1;
    };
    [[nodiscard]] Session session() const;

    // Reopens what it can: each path goes through open_path, and a
    // failure lands in `failures` (given) without stopping the walk.
    // The best-effort active index is restored when it survives.
    // Returns the number of documents reopened.
    [[nodiscard]] int restore_session(const Session& session,
                                      std::vector<std::string>* failures);

    // --- the autosave pair ------------------------------------------------

    // Snapshots the active document's autosave_content() (a no-op for
    // a document with none, or when autosave is disabled).
    [[nodiscard]] ws::core::Status snapshot_active();

    // The clean-exit half for one document (its real save landed).
    // No-op when autosave is disabled.
    [[nodiscard]] ws::core::Status finish_autosave(std::size_t index);

    // One leftover crash record found at startup, for the recovery
    // prompt: the document it belongs to (by session path) and where
    // the content waits.
    struct RecoveryOffer {
        std::string document;  // the path the session knew it by
        std::string autosave;  // where the content waits
    };

    // Probes the autosave store for the session's paths (the crash
    // evidence); each found marker becomes an offer. Nothing probes
    // when autosave is disabled.
    [[nodiscard]] std::vector<RecoveryOffer> probe_recovery(
        const std::vector<std::string>& paths) const;

    // Hands the crashed content back and clears the pair. The caller
    // adopts the content into a document it opens itself (the shell
    // routes it through open_path + adopt_autosave_content).
    [[nodiscard]] ws::core::Result<std::string> take_recovery(
        const std::string& document_path);

    // Drops one crashed autosave without recovering it.
    [[nodiscard]] ws::core::Status discard_recovery(
        const std::string& document_path);

private:
    // One open document plus its autosave pair. The pair is present
    // only when autosave is enabled AND the document has a key (a
    // path, or an untitled marker) AND it carries content.
    struct Entry {
        std::unique_ptr<Document> document;
        std::unique_ptr<ws::platform::Autosave> autosave;
    };
    [[nodiscard]] ws::platform::Autosave* autosave_for(std::size_t index,
                                                       bool create);
    [[nodiscard]] static std::string untitled_key(std::size_t index);

    DocumentFactory factory_;
    ws::settings::FileSystem* fs_;
    std::string autosave_dir_;
    std::vector<Entry> documents_;
    int active_ = -1;
};

}  // namespace ws::ui::shell
