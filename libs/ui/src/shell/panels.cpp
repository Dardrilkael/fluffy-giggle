// The panel framework's out-of-line half (panels.hpp's contract):
// registry validation, the instance bookkeeping over the dock tree,
// and the focus callbacks. Owner-thread throughout, no locks.

#include <ws/ui/shell/panels.hpp>

#include <algorithm>
#include <utility>

#include <ws/ui/shell/errors.hpp>

namespace ws::ui::shell {

namespace {

// The floating floor: a window smaller than the minimums is a title
// bar with no body, so every rect the layer accepts lands clamped
// (the drag, the resize and the restored file all pass through here
// - one clamp, one invariant).
[[nodiscard]] Rect clamped_float_rect(Rect rect) {
    rect.width = std::max(rect.width, kFloatingMinW);
    rect.height = std::max(rect.height, kFloatingMinH);
    return rect;
}

}  // namespace

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
    // The floating half of the same act: a torn-off window closes like
    // a tab (the id lives in exactly one home, but close() must not
    // care which).
    floating_.erase(std::remove_if(floating_.begin(), floating_.end(),
                                   [&](const FloatingPanel& win) {
                                       return win.panel == closing.id;
                                   }),
                    floating_.end());
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
    // Copy first: the id may live inside the floating vector this call
    // erases from (the view passes ids it read straight out of
    // floating()).
    const std::string id{instance};
    if (find(id) == nullptr) {
        return;
    }
    bool was_floating = false;
    for (std::size_t i = 0; i < floating_.size(); ++i) {
        if (floating_[i].panel == id) {
            floating_.erase(floating_.begin() +
                            static_cast<std::ptrdiff_t>(i));
            was_floating = true;
            break;
        }
    }
    if (was_floating) {
        // The way back in: the tree's add() (move() is the docked
        // half's primitive - a floating panel holds no tab to move).
        dock_.add(id, placement);
    } else {
        dock_.move(id, placement);
    }
    activate(registry, id);
}

void PanelHost::retab(const PanelRegistry& registry,
                      std::string_view instance, int leaf, int index) {
    if (find(instance) == nullptr) {
        return;
    }
    dock_.move_tab(instance, leaf, index);
    activate(registry, instance);
}

// --- the floating layer ---------------------------------------------------------

bool PanelHost::is_floating(std::string_view instance) const {
    for (const FloatingPanel& win : floating_) {
        if (win.panel == instance) {
            return true;
        }
    }
    return false;
}

void PanelHost::float_panel(const PanelRegistry& registry,
                            std::string_view instance, const Rect& rect) {
    // Copy first, the close() rule: the view passes ids it read out of
    // floating()/instances_, and the erase below can kill the very
    // string the view points into.
    const std::string id{instance};
    if (find(id) == nullptr) {
        return;  // an unknown id floats nowhere
    }
    const Rect clamped = clamped_float_rect(rect);
    for (std::size_t i = 0; i < floating_.size(); ++i) {
        if (floating_[i].panel == id) {
            // Already floating: the rect is taken and the window
            // raised - one id, one window, wherever it was asked for.
            FloatingPanel win = std::move(floating_[i]);
            win.rect = clamped;
            floating_.erase(floating_.begin() +
                            static_cast<std::ptrdiff_t>(i));
            floating_.push_back(std::move(win));
            activate(registry, id);
            return;
        }
    }
    FloatingPanel win;
    win.panel = id;
    win.rect = clamped;
    dock_.remove(id);  // the tree forgets the tab (a no-op when it
                       // held none - a floating id re-floating)
    floating_.push_back(std::move(win));
    activate(registry, id);  // the focus follows the panel out
}

void PanelHost::move_floating(std::string_view instance, const Rect& rect) {
    const std::string id{instance};
    for (FloatingPanel& win : floating_) {
        if (win.panel == id) {
            win.rect = clamped_float_rect(rect);
            return;
        }
    }
    // A docked or unknown id: nothing to move (the caller's own
    // bookkeeping decides who may drag what; the layer just holds).
}

void PanelHost::raise_floating(std::string_view instance) {
    const std::string id{instance};
    for (std::size_t i = 0; i < floating_.size(); ++i) {
        if (floating_[i].panel == id) {
            if (i + 1 == floating_.size()) {
                return;  // already the top of the stack
            }
            FloatingPanel win = std::move(floating_[i]);
            floating_.erase(floating_.begin() +
                            static_cast<std::ptrdiff_t>(i));
            floating_.push_back(std::move(win));
            return;
        }
    }
}

void PanelHost::clear_floating() { floating_.clear(); }

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
    // Dock order first (the historic walk), the floating windows after
    // it in z order; the ids are collected first because close()
    // mutates. Unfocus up front: tearing everything down must not
    // refocus a neighbour (and fire its on_focus) once per panel.
    active_.clear();
    std::vector<std::string> ids = dock_.panels();
    for (const FloatingPanel& win : floating_) {
        ids.push_back(win.panel);
    }
    for (const std::string& id : ids) {
        close(registry, id);
    }
    active_.clear();
    dock_.clear();
    floating_.clear();
}

// --- the layout json ---------------------------------------------------------

ws::settings::json::Value PanelHost::layout_to_json() const {
    namespace json = ws::settings::json;
    json::Value root = dock_.to_json();
    // The layer rides the root object as ONE extra key: the tree's own
    // shape stays byte-for-byte what dock.hpp documents, so an older
    // build reading the file still gets its whole tree (and a hand
    // edit that deletes the key loses only the windows).
    if (json::Object* obj = root.as_object()) {
        json::Array windows;
        for (const FloatingPanel& win : floating_) {
            json::Object entry;
            entry.set("id", json::Value::text(win.panel));
            entry.set("x", json::Value::real(win.rect.x));
            entry.set("y", json::Value::real(win.rect.y));
            entry.set("w", json::Value::real(win.rect.width));
            entry.set("h", json::Value::real(win.rect.height));
            windows.push_back(json::Value::object(std::move(entry)));
        }
        obj->set("floating", json::Value::array(std::move(windows)));
    }
    return root;
}

bool PanelHost::layout_from_json(
    const ws::settings::json::Value& value,
    const std::function<bool(std::string_view panel)>& accept) {
    namespace json = ws::settings::json;
    // The tree first, under its own tolerance: false means nothing
    // moved anywhere (dock_'s rule for a non-object root).
    if (!dock_.from_json(value, accept)) {
        return false;
    }
    floating_.clear();
    const json::Object* obj = value.as_object();
    const json::Value* windows =
        obj == nullptr ? nullptr : obj->find("floating");
    if (windows == nullptr || !windows->is_array()) {
        return true;  // a file with no layer restores no windows
    }
    for (const json::Value& entry : *windows->as_array()) {
        const json::Object* row = entry.as_object();
        if (row == nullptr) {
            continue;  // not a window entry: dropped, like the tree's
                       // own tolerance drops a bad child
        }
        const json::Value* id = row->find("id");
        const std::string* text = id == nullptr ? nullptr : id->as_text();
        if (text == nullptr || text->empty()) {
            continue;
        }
        if (accept != nullptr && !accept(*text)) {
            continue;  // the caller no longer knows this panel
        }
        if (find(*text) == nullptr) {
            continue;  // not open: a panel must not resurrect
        }
        if (dock_.contains(*text) || is_floating(*text)) {
            continue;  // one instance, one home - the first sighting
                       // wins, the tree's own dedupe rule
        }
        const auto number = [row](const char* key, float fallback) {
            const json::Value* v = row->find(key);
            if (v == nullptr || !v->is_real()) {
                return fallback;
            }
            return static_cast<float>(v->get_real().value_or(fallback));
        };
        FloatingPanel win;
        win.panel = *text;
        win.rect = clamped_float_rect(Rect{number("x", 0.0f),
                                           number("y", 0.0f),
                                           number("w", kFloatingMinW),
                                           number("h", kFloatingMinH)});
        floating_.push_back(std::move(win));
    }
    return true;
}

}  // namespace ws::ui::shell
