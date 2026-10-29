// The dock host's out-of-line half (dock.hpp's contract): the split
// tree, its resolve walk and its json persistence. Pure data + math -
// no widget, no draw call, no clock; the view in shell.cpp owns the
// pixels, and the tests own the geometry.

#include <ws/ui/shell/dock.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace ws::ui::shell {

// A node is either a split (children + weights) or a leaf (tabs +
// active); `split` picks the reading. Private on purpose: callers see
// LeafRect/SplitterRect and the mutators.
struct DockTree::Node {
    bool split = false;
    DockDir dir = DockDir::horizontal;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<float> weights;      // parallel to children, >= kMinWeight
    std::vector<std::string> tabs;   // leaf: the docked panel ids
    int active = 0;                  // leaf: the front tab (0 when empty)
};

DockTree::DockTree() : root_(std::make_unique<Node>()) {}

DockTree::DockTree(DockTree&&) noexcept = default;
DockTree& DockTree::operator=(DockTree&&) noexcept = default;
DockTree::~DockTree() = default;

void DockTree::collect_leaves(const Node& node,
                              std::vector<const Node*>& out) {
    if (!node.split) {
        out.push_back(&node);
        return;
    }
    for (const std::unique_ptr<Node>& child : node.children) {
        collect_leaves(*child, out);
    }
}

std::vector<const DockTree::Node*> DockTree::leaves_of(const Node& root) {
    std::vector<const Node*> out;
    collect_leaves(root, out);
    return out;
}

DockTree::Node* DockTree::leaf_at_index(Node& root, int index) {
    std::vector<Node*> out;
    const std::function<void(Node&)> walk = [&](Node& n) {
        if (!n.split) {
            out.push_back(&n);
            return;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            walk(*child);
        }
    };
    walk(root);
    if (out.empty()) {
        return nullptr;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= out.size()) {
        return out.back();
    }
    return out[static_cast<std::size_t>(index)];
}

void DockTree::prune(Node& node) {
    if (!node.split) {
        return;
    }
    // Depth-first: prune the children's subtrees, then drop whatever
    // became an empty leaf or a starving split at THIS level.
    for (const std::unique_ptr<Node>& child : node.children) {
        prune(*child);
    }
    for (std::size_t i = 0; i < node.children.size();) {
        const Node& child = *node.children[i];
        const bool empty_leaf = !child.split && child.tabs.empty();
        const bool starving_split = child.split && child.children.size() < 2;
        if (empty_leaf || starving_split) {
            node.children.erase(node.children.begin() +
                                static_cast<std::ptrdiff_t>(i));
            node.weights.erase(node.weights.begin() +
                               static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    // The collapse: a split left holding one child is replaced by it.
    if (node.children.size() == 1) {
        std::unique_ptr<Node> only = std::move(node.children.front());
        node = std::move(*only);
    }
}

void DockTree::add(std::string_view panel, DockPlacement placement) {
    if (panel.empty()) {
        return;  // the shell never hands an empty id; a no-op beats a crash
    }
    if (contains(panel)) {
        return;  // already docked: the caller activates instead
    }
    Node* target = leaf_at_index(*root_, placement.leaf);
    if (target == nullptr || target->split) {
        return;
    }
    const std::string id{panel};
    if (placement.area == DockPlacement::Area::center) {
        target->tabs.push_back(id);
        target->active = static_cast<int>(target->tabs.size()) - 1;
        return;
    }
    // A side placement: the target leaf's payload moves into one side
    // of a fresh split, a new leaf takes the other. left/right split
    // horizontally (side by side), top/bottom vertically.
    auto fresh = std::make_unique<Node>();
    fresh->tabs.push_back(id);
    fresh->active = 0;

    auto moved = std::make_unique<Node>(std::move(*target));
    Node& split = *target;  // the node object is reused as the split
    split.split = true;
    split.dir = (placement.area == DockPlacement::Area::left ||
                 placement.area == DockPlacement::Area::right)
                    ? DockDir::horizontal
                    : DockDir::vertical;
    const bool before = placement.area == DockPlacement::Area::left ||
                        placement.area == DockPlacement::Area::top;
    split.children.clear();
    split.weights.clear();
    if (before) {
        split.children.push_back(std::move(fresh));
        split.children.push_back(std::move(moved));
    } else {
        split.children.push_back(std::move(moved));
        split.children.push_back(std::move(fresh));
    }
    split.weights.assign(split.children.size(), 1.0f);
}

void DockTree::remove(std::string_view panel) {
    const std::function<bool(Node&)> walk = [&](Node& n) {
        if (!n.split) {
            for (std::size_t i = 0; i < n.tabs.size(); ++i) {
                if (n.tabs[i] == panel) {
                    n.tabs.erase(n.tabs.begin() +
                                 static_cast<std::ptrdiff_t>(i));
                    if (n.active >= static_cast<int>(n.tabs.size())) {
                        n.active = n.tabs.empty()
                                       ? 0
                                       : static_cast<int>(n.tabs.size()) - 1;
                    }
                    return true;
                }
            }
            return false;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            if (walk(*child)) {
                return true;
            }
        }
        return false;
    };
    if (!walk(*root_)) {
        return;
    }
    if (root_->split) {
        prune(*root_);
    }
    if (!root_->split && root_->tabs.empty()) {
        root_->active = 0;  // the empty root leaf stays - the invariant
    }
}

void DockTree::activate(std::string_view panel) {
    // The mutable walk (leaves_of answers const pointers): find the
    // leaf holding the tab, set its front tab.
    const std::function<bool(Node&)> walk = [&](Node& n) {
        if (!n.split) {
            for (std::size_t i = 0; i < n.tabs.size(); ++i) {
                if (n.tabs[i] == panel) {
                    n.active = static_cast<int>(i);
                    return true;
                }
            }
            return false;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            if (walk(*child)) {
                return true;
            }
        }
        return false;
    };
    (void)walk(*root_);
}

bool DockTree::contains(std::string_view panel) const {
    for (const Node* leaf : leaves_of(*root_)) {
        for (const std::string& tab : leaf->tabs) {
            if (tab == panel) {
                return true;
            }
        }
    }
    return false;
}

DockTree::Layout DockTree::resolve(const Rect& bounds) const {
    Layout out;
    int next_leaf = 0;

    const std::function<void(const Node&, const Rect&, const std::vector<int>&)>
        walk = [&](const Node& node, const Rect& rect,
                   const std::vector<int>& path) {
            if (!node.split) {
                LeafRect leaf;
                leaf.leaf = next_leaf++;
                leaf.rect = rect;
                leaf.tabs = &node.tabs;
                leaf.active = node.tabs.empty() ? 0 : node.active;
                out.leaves.push_back(leaf);
                return;
            }
            float total = 0.0f;
            for (float w : node.weights) {
                total += std::max(kMinWeight, w);
            }
            if (total <= 0.0f) {
                total = 1.0f;
            }
            const bool horizontal = node.dir == DockDir::horizontal;
            const float span = horizontal ? rect.width : rect.height;
            float at = horizontal ? rect.x : rect.y;
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                const float share = std::max(kMinWeight, node.weights[i]) / total;
                const float size = span * share;
                Rect child = rect;
                if (horizontal) {
                    child.x = at;
                    child.width = size;
                } else {
                    child.y = at;
                    child.height = size;
                }
                std::vector<int> child_path = path;
                child_path.push_back(static_cast<int>(i));
                walk(*node.children[i], child, child_path);
                at += size;

                // The splitter between this child and the next: a band
                // centered on the boundary, half in each side. One per
                // ADJACENT pair (children-1 of them).
                if (i + 1 < node.children.size()) {
                    const float half = kSplitterPx * 0.5f;
                    SplitterRect splitter;
                    splitter.path = path;  // the split node's own path
                    splitter.first_child = i;
                    splitter.dir = node.dir;
                    splitter.span_start = horizontal ? rect.x : rect.y;
                    splitter.span_size = span;
                    if (horizontal) {
                        splitter.rect =
                            Rect{at - half, rect.y, kSplitterPx, rect.height};
                    } else {
                        splitter.rect =
                            Rect{rect.x, at - half, rect.width, kSplitterPx};
                    }
                    out.splitters.push_back(std::move(splitter));
                }
            }
        };
    walk(*root_, bounds, {});
    return out;
}

void DockTree::apply_drag(const SplitterRect& splitter, float position) {
    Node* node = root_.get();
    for (int index : splitter.path) {
        if (node == nullptr || !node->split ||
            static_cast<std::size_t>(index) >= node->children.size()) {
            return;
        }
        node = node->children[static_cast<std::size_t>(index)].get();
    }
    if (node == nullptr || !node->split ||
        splitter.first_child + 1 >= node->children.size()) {
        return;
    }
    const std::size_t a = splitter.first_child;
    const std::size_t b = a + 1;
    // The pair's combined weight, and the WHOLE split's: the clamp
    // bounds the pair's NORMALIZED SHARES (what resolve() hands each
    // child), not the raw weights - a pair holding most of a split
    // must still leave its sibling kMinWeight of the split.
    const float pair = std::max(kMinWeight, node->weights[a]) +
                       std::max(kMinWeight, node->weights[b]);
    float whole = 0.0f;
    for (float w : node->weights) {
        whole += std::max(kMinWeight, w);
    }
    if (whole <= 0.0f) {
        whole = 1.0f;
    }
    const float span = splitter.span_size > 0.0f ? splitter.span_size : 1.0f;
    float frac = (position - splitter.span_start) / span;
    const float lo = std::min(0.5f, kMinWeight * whole / pair);
    const float hi = 1.0f - lo;
    frac = std::max(lo, std::min(hi, frac));
    node->weights[a] = pair * frac;
    node->weights[b] = pair * (1.0f - frac);
}

int DockTree::leaf_of(std::string_view panel) const {
    const std::vector<const Node*> leaves = leaves_of(*root_);
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        for (const std::string& tab : leaves[i]->tabs) {
            if (tab == panel) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

int DockTree::leaf_count() const {
    return static_cast<int>(leaves_of(*root_).size());
}

std::vector<std::string> DockTree::panels() const {
    std::vector<std::string> out;
    for (const Node* leaf : leaves_of(*root_)) {
        out.insert(out.end(), leaf->tabs.begin(), leaf->tabs.end());
    }
    return out;
}

// --- the json halves ---------------------------------------------------------

ws::settings::json::Value DockTree::node_to_json(const Node& node) {
    namespace json = ws::settings::json;
    if (!node.split) {
        json::Object obj;
        json::Array tabs;
        for (const std::string& tab : node.tabs) {
            tabs.push_back(json::Value::text(tab));
        }
        obj.set("tabs", json::Value::array(std::move(tabs)));
        obj.set("active", json::Value::integer(node.active));
        return json::Value::object(std::move(obj));
    }
    float total = 0.0f;
    for (float w : node.weights) {
        total += std::max(kMinWeight, w);
    }
    if (total <= 0.0f) {
        total = 1.0f;
    }
    json::Object obj;
    obj.set("dir",
            json::Value::text(node.dir == DockDir::horizontal ? "h" : "v"));
    json::Array children;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        json::Object child;
        child.set(
            "w",
            json::Value::real(std::max(kMinWeight, node.weights[i]) / total));
        child.set("node", DockTree::node_to_json(*node.children[i]));
        children.push_back(json::Value::object(std::move(child)));
    }
    obj.set("children", json::Value::array(std::move(children)));
    return json::Value::object(std::move(obj));
}

std::unique_ptr<DockTree::Node> DockTree::node_from_json(
    const ws::settings::json::Value& value,
    const std::function<bool(std::string_view)>& accept) {
    namespace json = ws::settings::json;
    const json::Object* obj = value.as_object();
    if (obj == nullptr) {
        return nullptr;
    }
    const json::Value* children = obj->find("children");
    if (children == nullptr || !children->is_array()) {
        // The leaf shape: a "tabs" array (the accept filter drops the
        // ids the caller no longer knows).
        const json::Value* tabs = obj->find("tabs");
        if (tabs == nullptr || !tabs->is_array()) {
            return nullptr;
        }
        auto node = std::make_unique<Node>();
        for (const json::Value& tab : *tabs->as_array()) {
            const std::string* text = tab.as_text();
            if (text == nullptr || text->empty() ||
                (accept && !accept(*text))) {
                continue;
            }
            node->tabs.push_back(*text);
        }
        const json::Value* active = obj->find("active");
        if (active != nullptr && active->is_integer() && !node->tabs.empty()) {
            const std::int64_t want = active->get_integer().value_or(0);
            node->active = static_cast<int>(std::max<std::int64_t>(
                0, std::min<std::int64_t>(
                       want, static_cast<std::int64_t>(node->tabs.size()) - 1)));
        }
        return node;
    }
    const json::Value* dir = obj->find("dir");
    const bool horizontal =
        dir != nullptr && dir->is_text() && *dir->as_text() == "h";

    auto node = std::make_unique<Node>();
    node->split = true;
    node->dir = horizontal ? DockDir::horizontal : DockDir::vertical;
    for (const json::Value& child : *children->as_array()) {
        const json::Object* wrap = child.as_object();
        const json::Value* wrapped =
            wrap == nullptr ? &child : wrap->find("node");
        if (wrapped == nullptr) {
            wrapped = &child;
        }
        std::unique_ptr<Node> parsed = node_from_json(*wrapped, accept);
        if (parsed == nullptr) {
            continue;
        }
        // The empty-leaf invariant: a leaf whose every tab the accept
        // filter refused is dropped here, not kept as dead chrome.
        if (!parsed->split && parsed->tabs.empty()) {
            continue;
        }
        float weight = 1.0f;
        if (wrap != nullptr) {
            const json::Value* w = wrap->find("w");
            if (w != nullptr && w->is_real() && w->get_real().value_or(1.0) > 0.0) {
                weight = static_cast<float>(w->get_real().value_or(1.0));
            }
        }
        node->children.push_back(std::move(parsed));
        node->weights.push_back(std::max(kMinWeight, weight));
    }
    // The invariants: >= 2 children, or not a split at all.
    if (node->children.empty()) {
        return nullptr;
    }
    if (node->children.size() == 1) {
        return std::move(node->children.front());
    }
    return node;
}

ws::settings::json::Value DockTree::to_json() const {
    return node_to_json(*root_);
}

bool DockTree::from_json(const ws::settings::json::Value& value,
                         const std::function<bool(std::string_view)>& accept) {
    std::unique_ptr<Node> parsed = node_from_json(value, accept);
    if (parsed == nullptr) {
        return false;
    }
    root_ = std::move(parsed);
    if (!root_->split && root_->tabs.empty()) {
        root_->active = 0;
    }
    return true;
}

void DockTree::clear() { root_ = std::make_unique<Node>(); }

}  // namespace ws::ui::shell
