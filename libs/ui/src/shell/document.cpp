// The document layer's out-of-line half (document.hpp's contract):
// the open set, the close flow, session restore and the per-document
// autosave pair. Owner-thread throughout, no locks.

#include <ws/ui/shell/document.hpp>

#include <algorithm>
#include <utility>

#include <ws/ui/shell/errors.hpp>

namespace ws::ui::shell {
namespace {

using Document = ws::ui::shell::Document;

}  // namespace

DocumentManager::DocumentManager(DocumentFactory factory,
                                 ws::settings::FileSystem* fs,
                                 std::string autosave_dir)
    : factory_(std::move(factory)),
      fs_(fs),
      autosave_dir_(std::move(autosave_dir)) {}

DocumentManager::~DocumentManager() = default;

ws::core::Result<int> DocumentManager::open_path(const std::string& path) {
    // The same file twice: activate, do not duplicate.
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i].document != nullptr &&
            documents_[i].document->path() == path) {
            activate(i);
            return static_cast<int>(i);
        }
    }
    if (!factory_) {
        return ws::core::unexpected(
            shell_error(ShellError::unavailable, "no document factory set"));
    }
    ws::core::Result<std::unique_ptr<Document>> made = factory_(path);
    if (!made) {
        return ws::core::unexpected(made.error());
    }
    std::unique_ptr<Document> document = std::move(*made);
    if (!document) {
        return ws::core::unexpected(
            shell_error(ShellError::document, "factory made no document for '" +
                                                  path + "'"));
    }
    if (const ws::core::Status st = document->load(); !st) {
        return ws::core::unexpected(st.error());
    }
    (void)document->undo().mark_saved();
    const int index = adopt(std::move(document));
    if (on_opened_path) {
        on_opened_path(documents_[static_cast<std::size_t>(index)]
                           .document->doc_type(),
                       path);
    }
    return index;
}

int DocumentManager::adopt(std::unique_ptr<Document> document) {
    Entry entry;
    entry.document = std::move(document);
    documents_.push_back(std::move(entry));
    activate(documents_.size() - 1);
    return static_cast<int>(documents_.size()) - 1;
}

ws::core::Result<int> DocumentManager::new_document() {
    if (!factory_) {
        return ws::core::unexpected(
            shell_error(ShellError::unavailable, "no document factory set"));
    }
    ws::core::Result<std::unique_ptr<Document>> made = factory_("");
    if (!made) {
        return ws::core::unexpected(made.error());
    }
    std::unique_ptr<Document> document = std::move(*made);
    if (!document) {
        return ws::core::unexpected(shell_error(
            ShellError::document, "factory made no untitled document"));
    }
    (void)document->undo().mark_saved();
    return adopt(std::move(document));
}

DocumentManager::ClosePlan DocumentManager::plan_close(std::size_t index) {
    ClosePlan plan;
    plan.index = index;
    plan.valid = index < documents_.size();
    if (!plan.valid) {
        return plan;
    }
    // (non-const: is_dirty() rides the undo stack, not a const probe)
    Document* document = documents_[index].document.get();
    plan.prompt = document != nullptr && document->undo().is_dirty();
    return plan;
}

ws::core::Status DocumentManager::finish_close(std::size_t index,
                                               bool save_first) {
    if (index >= documents_.size()) {
        return ws::core::unexpected(shell_error(
            ShellError::invalid, "close: no document " + std::to_string(index)));
    }
    Document* document = documents_[index].document.get();
    if (document == nullptr) {
        return ws::core::unexpected(
            shell_error(ShellError::invalid, "close: null document"));
    }
    if (save_first) {
        if (const ws::core::Status st = document->save(); !st) {
            return st;  // still open: the failure is the caller's to render
        }
        (void)document->undo().mark_saved();
    }
    // The autosave pair ends with the document: a saved one finished
    // (marker cleared), a discarded one dropped wholesale.
    if (documents_[index].autosave != nullptr) {
        if (save_first) {
            (void)documents_[index].autosave->finish();
        } else {
            (void)documents_[index].autosave->discard();
        }
    }
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
    if (documents_.empty()) {
        active_ = -1;
    } else if (static_cast<int>(index) <= active_) {
        active_ = std::max(0, active_ - 1);
    }
    return {};
}

Document* DocumentManager::at(std::size_t index) {
    if (index >= documents_.size()) {
        return nullptr;
    }
    return documents_[index].document.get();
}

const Document* DocumentManager::at(std::size_t index) const {
    if (index >= documents_.size()) {
        return nullptr;
    }
    return documents_[index].document.get();
}

Document* DocumentManager::active() {
    return active_ < 0 ? nullptr : at(static_cast<std::size_t>(active_));
}

const Document* DocumentManager::active() const {
    return active_ < 0 ? nullptr : at(static_cast<std::size_t>(active_));
}

void DocumentManager::activate(std::size_t index) {
    if (index < documents_.size()) {
        active_ = static_cast<int>(index);
    }
}

DocumentManager::Session DocumentManager::session() const {
    Session out;
    for (const Entry& entry : documents_) {
        if (entry.document != nullptr) {
            out.paths.push_back(entry.document->path());
        } else {
            out.paths.push_back({});
        }
    }
    out.active = active_;
    return out;
}

int DocumentManager::restore_session(const Session& session,
                                     std::vector<std::string>* failures) {
    int reopened = 0;
    for (const std::string& path : session.paths) {
        if (path.empty()) {
            continue;  // an untitled document is not session state
        }
        const ws::core::Result<int> index = open_path(path);
        if (index) {
            ++reopened;
        } else if (failures != nullptr) {
            failures->push_back(path + ": " + index.error().message);
        }
    }
    if (session.active >= 0 &&
        static_cast<std::size_t>(session.active) < documents_.size()) {
        activate(static_cast<std::size_t>(session.active));
    }
    return reopened;
}

ws::platform::Autosave* DocumentManager::autosave_for(std::size_t index,
                                                       bool create) {
    if (fs_ == nullptr || autosave_dir_.empty() ||
        index >= documents_.size() || documents_[index].document == nullptr) {
        return nullptr;
    }
    Entry& entry = documents_[index];
    if (entry.autosave != nullptr || !create) {
        return entry.autosave.get();
    }
    // The store key: the document's own path, or the untitled marker
    // (a stable per-process key; an untitled doc's crash content is
    // offered by path "untitled-N" - the recovery prompt names it).
    const std::string key = entry.document->path().empty()
                                ? untitled_key(index)
                                : entry.document->path();
    entry.autosave = std::make_unique<ws::platform::Autosave>(
        *fs_, key, autosave_dir_);
    return entry.autosave.get();
}

std::string DocumentManager::untitled_key(std::size_t index) {
    return "untitled-" + std::to_string(index + 1);
}

ws::core::Status DocumentManager::snapshot_active() {
    Document* document = active();
    if (document == nullptr) {
        return {};
    }
    const std::string content = document->autosave_content();
    if (content.empty()) {
        return {};  // no content seam: nothing to snapshot
    }
    ws::platform::Autosave* autosave =
        autosave_for(static_cast<std::size_t>(active_), true);
    if (autosave == nullptr) {
        return {};  // disabled: the headless default
    }
    return autosave->snapshot(content);
}

ws::core::Status DocumentManager::finish_autosave(std::size_t index) {
    if (index >= documents_.size()) {
        return {};
    }
    ws::platform::Autosave* autosave = autosave_for(index, false);
    if (autosave == nullptr) {
        return {};
    }
    return autosave->finish();
}

std::vector<DocumentManager::RecoveryOffer>
DocumentManager::probe_recovery(const std::vector<std::string>& paths) const {
    std::vector<RecoveryOffer> offers;
    if (fs_ == nullptr || autosave_dir_.empty()) {
        return offers;
    }
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue;
        }
        const ws::platform::Autosave probe(*fs_, path, autosave_dir_);
        const ws::core::Result<std::optional<ws::platform::Recovery>> found =
            probe.probe();
        if (found && found->has_value()) {
            RecoveryOffer offer;
            offer.document = path;
            offer.autosave = (*found)->autosave;
            offers.push_back(std::move(offer));
        }
    }
    return offers;
}

ws::core::Result<std::string> DocumentManager::take_recovery(
    const std::string& document_path) {
    if (fs_ == nullptr || autosave_dir_.empty()) {
        return ws::core::unexpected(
            shell_error(ShellError::unavailable, "autosave disabled"));
    }
    ws::platform::Autosave pair(*fs_, document_path, autosave_dir_);
    return pair.recover();
}

ws::core::Status DocumentManager::discard_recovery(
    const std::string& document_path) {
    if (fs_ == nullptr || autosave_dir_.empty()) {
        return {};
    }
    ws::platform::Autosave pair(*fs_, document_path, autosave_dir_);
    return pair.discard();
}

}  // namespace ws::ui::shell
