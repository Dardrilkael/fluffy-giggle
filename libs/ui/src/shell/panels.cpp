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
        return ws::core::unexpected<ws::core::Error>(shell_error(
            ShellError::invalid,
            "panel needs a non-empty id, a title and a draw callback: '" +
                desc.id + "'"));
    }
    if (contains(desc.id)) {
        return ws::core::unexpected<ws::core::Error>(shell_error(
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
        return ws::core::unexpected<ws::core::Error>(shell_error(
            ShellError::invalid, "unknown panel type: '" + std::string{type} + "'"));
    }
    if (!desc->multi_instance && is_open(type)) {
        return ws::core::unexpected<ws::core::Error>(shell_error(
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
    // "Editor 1", "Editor 2": tabs of one multi-instance type must be
    // tellable apart (the number is the id's own, so a title never
    // changes under the user).
    instance.title = desc->multi_instance
                         ? desc->title + " " + std::to_string(multi_counter_)
                         : desc->title;
    dock_.add(id, desc->placement);
    instances_.push_back(std::move(instance));
    activate(registry, id);
    return id;
}

bool PanelHost::close(const PanelRegistry& registry,
                      std::string_view instance) {
    const auto find_it = [this](const std::string& id) {
        return std::find_if(instances_.begin(), instances_.end(),
                            [&](const PanelInstance& p) { return p.id == id; });
    };
    // Copy first: `instance` may be a view INTO the element about to go
    // (close_type/close_all pass ids that live in the vectors).
    auto it = find_it(std::string{instance});
    if (it == instances_.end()) {
        return false;
    }
    const PanelInstance closing = *it;  // callbacks get a copy that cannot dangle
    const bool was_active = active_ == closing.id;

    if (const PanelDesc* desc = registry.find(closing.type);
        desc != nullptr && desc->on_close) {
        desc->on_close(closing);
    }
    // on_close may have opened or closed panels (even this one).
    it = find_it(closing.id);
    if (it == instances_.end()) {
        return true;  // the callback already closed it
    }
    const auto pos = static_cast<std::size_t>(it - instances_.begin());
    dock_.remove(closing.id);
    instances_.erase(it);
    if (was_active) {
        active_.clear();
        if (!instances_.empty()) {
            // Focus goes to the neighbour, like a tab strip: shortcuts
            // scoped to a panel must not go dead until the next click.
            activate(registry,
                     instances_[std::min(pos, instances_.size() - 1)].id);
        }
    }
    return true;
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
    // A copy: on_focus may open/close panels and reshape instances_.
    const PanelInstance focused = *found;
    const bool changed = active_ != focused.id;
    active_ = focused.id;
    dock_.activate(focused.id);
    if (changed) {
        const PanelDesc* desc = registry.find(focused.type);
        if (desc != nullptr && desc->on_focus) {
            desc->on_focus(focused);
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

void PanelHost::redock(const PanelRegistry& registry,
                       std::string_view instance, DockPlacement placement) {
    if (find(instance) == nullptr) {
        return;
    }
    dock_.move(instance, placement);
    activate(registry, instance);
}

void PanelHost::retab(const PanelRegistry& registry,
                      std::string_view instance, int leaf, int index) {
    if (find(instance) == nullptr) {
        return;
    }
    dock_.move_tab(instance, leaf, index);
    activate(registry, instance);
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
    // Unfocus up front: tearing everything down must not refocus a
    // neighbour (and fire its on_focus) once per panel.
    active_.clear();
    const std::vector<std::string> ids = dock_.panels();
    for (const std::string& id : ids) {
        close(registry, id);
    }
    active_.clear();
    dock_.clear();
}

}  // namespace ws::ui::shell
