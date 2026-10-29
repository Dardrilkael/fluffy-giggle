// The panel framework's out-of-line half (panels.hpp's contract):
// registry validation, the instance bookkeeping over the dock tree,
// and the focus callbacks. Owner-thread throughout, no locks.

#include <ws/ui/shell/panels.hpp>

#include <algorithm>
#include <utility>

#include <ws/ui/shell/errors.hpp>

namespace ws::ui::shell {

ws::core::Status PanelRegistry::add(PanelDesc desc) {
    if (desc.id.empty() || desc.title.empty() || !desc.draw) {
        return ws::core::unexpected(shell_error(
            ShellError::invalid,
            "panel needs a non-empty id, a title and a draw callback: '" +
                desc.id + "'"));
    }
    if (contains(desc.id)) {
        return ws::core::unexpected(shell_error(
            ShellError::invalid, "panel id already registered: '" + desc.id + "'"));
    }
    descs_.push_back(std::move(desc));
    return {};
}

const PanelDesc* PanelRegistry::find(std::string_view type) const {
    for (const PanelDesc& desc : descs_) {
        if (desc.id == type) {
            return &desc;
        }
    }
    return nullptr;
}

bool PanelRegistry::contains(std::string_view type) const {
    return find(type) != nullptr;
}

ws::core::Result<std::string> PanelHost::open(const PanelRegistry& registry,
                                              std::string_view type) {
    const PanelDesc* desc = registry.find(type);
    if (desc == nullptr) {
        return ws::core::unexpected(shell_error(
            ShellError::invalid, "unknown panel type: '" + std::string{type} + "'"));
    }
    if (!desc->multi_instance && is_open(type)) {
        return ws::core::unexpected(shell_error(
            ShellError::invalid,
            "panel already open (single-instance): '" + std::string{type} + "'"));
    }
    std::string id{type};
    if (desc->multi_instance) {
        // "editor:1", "editor:2"... the counter never reuses a
        // number, so a closed instance's widget state (keyed by id)
        // can never be confused with a reopened one's.
        do {
            id = std::string{type} + ":" + std::to_string(++multi_counter_);
        } while (dock_.contains(id));
    }
    PanelInstance instance;
    instance.id = id;
    instance.type = desc->id;
    instance.title = desc->title;
    dock_.add(id, desc->placement);
    instances_.push_back(std::move(instance));
    activate(registry, id);
    return id;
}

bool PanelHost::close(const PanelRegistry& registry,
                      std::string_view instance) {
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].id != instance) {
            continue;
        }
        const PanelDesc* desc = registry.find(instances_[i].type);
        if (desc != nullptr && desc->on_close) {
            desc->on_close(instances_[i]);
        }
        if (active_ == instance) {
            active_.clear();
        }
        dock_.remove(instance);
        instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void PanelHost::close_type(const PanelRegistry& registry,
                           std::string_view type) {
    // Collect first: closing mutates the vector mid-walk.
    std::vector<std::string> ids;
    for (const PanelInstance& instance : instances_) {
        if (instance.type == type) {
            ids.push_back(instance.id);
        }
    }
    for (const std::string& id : ids) {
        close(registry, id);
    }
}

bool PanelHost::is_open(std::string_view type) const {
    for (const PanelInstance& instance : instances_) {
        if (instance.type == type) {
            return true;
        }
    }
    return false;
}

const PanelInstance* PanelHost::find(std::string_view instance) const {
    for (const PanelInstance& live : instances_) {
        if (live.id == instance) {
            return &live;
        }
    }
    return nullptr;
}

void PanelHost::activate(const PanelRegistry& registry,
                         std::string_view instance) {
    const PanelInstance* found = find(instance);
    if (found == nullptr) {
        return;
    }
    const bool changed = active_ != found->id;
    active_ = found->id;
    dock_.activate(found->id);
    if (changed) {
        const PanelDesc* desc = registry.find(found->type);
        if (desc != nullptr && desc->on_focus) {
            desc->on_focus(*found);
        }
    }
}

void PanelHost::cycle(const PanelRegistry& registry) {
    if (instances_.empty()) {
        return;
    }
    std::size_t next = 0;
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].id == active_) {
            next = (i + 1) % instances_.size();
            break;
        }
    }
    activate(registry, instances_[next].id);
}

PanelInstance* PanelHost::active_instance() {
    for (PanelInstance& instance : instances_) {
        if (instance.id == active_) {
            return &instance;
        }
    }
    return nullptr;
}

const PanelInstance* PanelHost::active_instance() const {
    for (const PanelInstance& instance : instances_) {
        if (instance.id == active_) {
            return &instance;
        }
    }
    return nullptr;
}

void PanelHost::close_all(const PanelRegistry& registry) {
    // Dock order; the ids are collected first because close() mutates.
    const std::vector<std::string> ids = dock_.panels();
    for (const std::string& id : ids) {
        close(registry, id);
    }
    active_.clear();
    dock_.clear();
}

}  // namespace ws::ui::shell
