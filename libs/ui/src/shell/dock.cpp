// The dock host's out-of-line half (dock.hpp's contract): the split
// tree, its resolve walk and its json persistence. Pure data + math -
// no widget, no draw call, no clock; the view in shell.cpp owns the
// pixels, and the tests own the geometry.
//
// The placement machinery's one idea is FUSION: a side placement whose
// target sits in a split of the same axis lands as one more sibling
// of that split instead of a fresh split nested inside the leaf. The
// tree stays flat (splitters keep meaning "the boundary between these
// neighbors"), the json keeps its N-children shape, and the pixels are
// the ones the nested spelling would have drawn - the new leaf takes
// its share of the target's span either way.

#include <ws/ui/shell/dock.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>
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

void DockTree::tidy() {
    if (root_->split) {
        prune(*root_);
        if (root_->split && root_->children.empty()) {
            // A split that lost every child is not a tree add() can
            // land in (no leaf to find): back to the empty root leaf.
            root_ = std::make_unique<Node>();
        }
    }
    if (!root_->split && root_->tabs.empty()) {
        root_->active = 0;  // the empty root leaf stays - the invariant
    }
}

void DockTree::erase_tab(Node& leaf, std::size_t tab) {
    leaf.tabs.erase(leaf.tabs.begin() + static_cast<std::ptrdiff_t>(tab));
    if (static_cast<int>(tab) < leaf.active) {
        --leaf.active;  // an earlier tab left: same tab stays in front
    }
    if (leaf.active >= static_cast<int>(leaf.tabs.size())) {
        leaf.active = leaf.tabs.empty()
                          ? 0
                          : static_cast<int>(leaf.tabs.size()) - 1;
    }
}

DockTree::Node* DockTree::parent_of(Node& node, Node& child, int* index) {
    if (&node == &child || !node.split) {
        return nullptr;  // the root is its own nobody's child
    }
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (node.children[i].get() == &child) {
            if (index != nullptr) {
                *index = static_cast<int>(i);
            }
            return &node;
        }
    }
    for (const std::unique_ptr<Node>& kid : node.children) {
        if (Node* found = parent_of(*kid, child, index)) {
            return found;
        }
    }
    return nullptr;
}

DockTree::Node* DockTree::wrap(Node& node, DockDir dir, bool before,
                               float share, std::unique_ptr<Node> fresh) {
    Node* landed = fresh.get();
    auto moved = std::make_unique<Node>(std::move(node));
    Node& split = node;  // the node object is reused as the split
    split.split = true;
    split.dir = dir;
    split.tabs.clear();
    split.active = 0;
    split.children.clear();
    split.weights.clear();
    if (before) {
        split.children.push_back(std::move(fresh));
        split.children.push_back(std::move(moved));
        split.weights = {share, 1.0f - share};
    } else {
        split.children.push_back(std::move(moved));
        split.children.push_back(std::move(fresh));
        split.weights = {1.0f - share, share};
    }
    return landed;
}

DockTree::Node* DockTree::insert_leaf(Node& root, Node& target,
                                      DockPlacement placement,
                                      std::unique_ptr<Node> fresh) {
    const DockPlacement::Area area = placement.area;

    // The fills: center, or any side placement with nothing to side
    // against. Splitting empty chrome moves its (empty) payload into a
    // sibling that resolve() paints as a phantom pane of dead widgets
    // - the stage-15 ghost leaf hit this daily. The panel just fills
    // the leaf, the same answer center always gives.
    if (area == DockPlacement::Area::center ||
        (!target.split && target.tabs.empty())) {
        target.tabs.push_back(std::move(fresh->tabs.front()));
        target.active = static_cast<int>(target.tabs.size()) - 1;
        return &target;
    }

    const bool before = area == DockPlacement::Area::left ||
                        area == DockPlacement::Area::top;
    const DockDir dir =
        (area == DockPlacement::Area::left ||
         area == DockPlacement::Area::right)
            ? DockDir::horizontal
            : DockDir::vertical;
    // The new leaf's share of the target's span; 0 (unset) = the
    // even split.
    float share = placement.weight > 0.0f ? placement.weight : 0.5f;
    share = std::clamp(share, kMinWeight, 1.0f - kMinWeight);

    // The whole-tree placement: `target` is the root split (only the
    // at_root redirect hands a split here). A matching axis FUSES -
    // the new leaf spans the full width/height as one more root child,
    // every existing weight scaled by its cut, which is the same
    // relative shrink a wrap would hand them without the nesting; a
    // mismatched axis wraps the whole tree.
    if (target.split) {
        if (target.dir == dir) {
            float total = 0.0f;
            for (float w : target.weights) {
                total += std::max(kMinWeight, w);
            }
            if (total <= 0.0f) {
                total = 1.0f;
            }
            for (float& w : target.weights) {
                w = std::max(kMinWeight, w * (1.0f - share));
            }
            Node* landed = fresh.get();
            const std::size_t at = before ? 0 : target.children.size();
            target.children.insert(
                target.children.begin() +
                    static_cast<std::ptrdiff_t>(at),
                std::move(fresh));
            target.weights.insert(
                target.weights.begin() +
                    static_cast<std::ptrdiff_t>(at),
                std::max(kMinWeight, total * share));
            return landed;
        }
        return wrap(target, dir, before, share, std::move(fresh));
    }

    // A leaf target with payload: fuse into the parent split when its
    // axis matches. The new leaf takes its share of the TARGET's span
    // - the target child's weight splits, the siblings keep theirs -
    // so the pixels are exactly the nested spelling's, one flatter
    // tree (the splitter between the pair is the boundary the drag
    // moves, with no intermediate split to confuse it).
    int target_at = -1;
    Node* parent = parent_of(root, target, &target_at);
    if (parent != nullptr && parent->dir == dir) {
        const float span =
            std::max(kMinWeight,
                     parent->weights[static_cast<std::size_t>(target_at)]);
        parent->weights[static_cast<std::size_t>(target_at)] =
            std::max(kMinWeight, span * (1.0f - share));
        const std::size_t at = before
                                   ? static_cast<std::size_t>(target_at)
                                   : static_cast<std::size_t>(target_at) + 1;
        Node* landed = fresh.get();
        parent->children.insert(
            parent->children.begin() + static_cast<std::ptrdiff_t>(at),
            std::move(fresh));
        parent->weights.insert(
            parent->weights.begin() + static_cast<std::ptrdiff_t>(at),
            std::max(kMinWeight, span * share));
        return landed;
    }

    // No matching split to join: the target's payload moves into one
    // side of a fresh split, the new leaf takes the other. left/right
    // split horizontally (side by side), top/bottom vertically.
    return wrap(target, dir, before, share, std::move(fresh));
}

DockTree::Node* DockTree::node_at(const std::vector<int>& path) {
    Node* node = root_.get();
    for (int index : path) {
        if (node == nullptr || !node->split ||
            static_cast<std::size_t>(index) >= node->children.size()) {
            return nullptr;
        }
        node = node->children[static_cast<std::size_t>(index)].get();
    }
    return node;
}

void DockTree::add(std::string_view panel, DockPlacement placement) {
    if (panel.empty()) {
        return;  // the shell never hands an empty id; a no-op beats a crash
    }
    if (contains(panel)) {
        return;  // already docked: the caller re-docks through move()
    }
    Node* target = leaf_at_index(*root_, placement.leaf);
    if (target == nullptr) {
        return;
    }
    auto fresh = std::make_unique<Node>();
    fresh->tabs.emplace_back(panel);
    fresh->active = 0;
    // A whole-tree split: only for a side placement on a tree that
    // already holds something (an empty tree fills, like any side
    // placement with nothing to side against).
    Node& dst = placement.at_root &&
                        placement.area != DockPlacement::Area::center &&
                        (root_->split || !root_->tabs.empty())
                    ? *root_
                    : *target;
    (void)insert_leaf(*root_, dst, placement, std::move(fresh));
}

void DockTree::move(std::string_view panel, DockPlacement placement) {
    // The source: the leaf holding the tab, and the tab's slot in it.
    Node* src = nullptr;
    std::size_t tab = 0;
    const std::function<bool(Node&)> find = [&](Node& n) {
        if (!n.split) {
            for (std::size_t i = 0; i < n.tabs.size(); ++i) {
                if (n.tabs[i] == panel) {
                    src = &n;
                    tab = i;
                    return true;
                }
            }
            return false;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            if (find(*child)) {
                return true;
            }
        }
        return false;
    };
    if (!find(*root_)) {
        return;  // docked nowhere: nothing to re-dock
    }
    Node* target = leaf_at_index(*root_, placement.leaf);
    if (target == nullptr) {
        return;
    }
    const std::string id{panel};
    if (placement.area == DockPlacement::Area::center) {
        if (target == src) {
            activate(panel);  // onto its own strip: a reorder, not a move
            return;
        }
        // The retab: out of the old leaf, the front tab of the new one
        // (a dropped tab is the selected tab).
        erase_tab(*src, tab);
        target->tabs.push_back(id);
        target->active = static_cast<int>(target->tabs.size()) - 1;
        tidy();
        return;
    }
    // A side placement. The at_root redirect and the leaf target share
    // insert_leaf; the only move-specific shape is the source landing
    // on itself.
    Node& dst = placement.at_root &&
                        (root_->split || !root_->tabs.empty())
                    ? *root_
                    : *target;
    if (&dst == src && src->tabs.size() == 1) {
        activate(panel);  // a leaf splitting around only itself: nothing
                          // to side against - the tab just fronts
        return;
    }
    auto fresh = std::make_unique<Node>();
    fresh->tabs.push_back(id);
    fresh->active = 0;
    if (&dst == src) {
        // Out of its own leaf FIRST: the wrap below moves the leaf's
        // remaining tabs as the split's other side, and the payload
        // must not ride along twice. The leaf keeps its other tabs,
        // so nothing prunes here.
        erase_tab(*src, tab);
        (void)insert_leaf(*root_, dst, placement, std::move(fresh));
    } else {
        // The new leaf first, the erase after: insert_leaf never moves
        // the source node (it wraps or fuses around the TARGET - the
        // wrap reuses the target's object, the fuse pushes unique_ptrs
        // whose pointees stay put), while the erase can empty the
        // source and the prune that follows re-shapes the tree.
        (void)insert_leaf(*root_, dst, placement, std::move(fresh));
        erase_tab(*src, tab);
    }
    tidy();
}

void DockTree::move_tab(std::string_view panel, int leaf, int index) {
    Node* src = nullptr;
    std::size_t tab = 0;
    const std::function<bool(Node&)> find = [&](Node& n) {
        if (!n.split) {
            for (std::size_t i = 0; i < n.tabs.size(); ++i) {
                if (n.tabs[i] == panel) {
                    src = &n;
                    tab = i;
                    return true;
                }
            }
            return false;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            if (find(*child)) {
                return true;
            }
        }
        return false;
    };
    if (!find(*root_)) {
        return;
    }
    Node* target = leaf_at_index(*root_, leaf);
    if (target == nullptr) {
        return;
    }
    const std::string id{panel};
    if (target == src) {
        // The reorder inside one strip: erase and reinsert, with the
        // front tab following its own move or stepping past whatever
        // slid in ahead of it.
        const bool was_front = static_cast<int>(tab) == src->active;
        const std::size_t count = src->tabs.size();
        src->tabs.erase(src->tabs.begin() +
                        static_cast<std::ptrdiff_t>(tab));
        if (static_cast<int>(tab) < src->active) {
            --src->active;
        }
        const int at = std::clamp(index, 0,
                                  static_cast<int>(count) - 1);
        src->tabs.insert(src->tabs.begin() +
                         static_cast<std::ptrdiff_t>(at), id);
        if (was_front) {
            src->active = at;
        } else if (at <= src->active) {
            ++src->active;
        }
        return;
    }
    // The retab: out of the old strip, into the new one at `index`,
    // and the dropped tab fronts its new leaf.
    erase_tab(*src, tab);
    const int at =
        std::clamp(index, 0, static_cast<int>(target->tabs.size()));
    target->tabs.insert(target->tabs.begin() +
                        static_cast<std::ptrdiff_t>(at), id);
    target->active = at;
    tidy();
}

void DockTree::remove(std::string_view panel) {
    Node* found = nullptr;
    std::size_t tab = 0;
    const std::function<bool(Node&)> walk = [&](Node& n) {
        if (!n.split) {
            for (std::size_t i = 0; i < n.tabs.size(); ++i) {
                if (n.tabs[i] == panel) {
                    found = &n;
                    tab = i;
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
    erase_tab(*found, tab);
    if (maximized_ == panel) {
        restore();  // the zoomed panel left the tree: no stale zoom
    }
    tidy();
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

    // The zoom projection: one leaf, the whole bounds, no splitters.
    // The tree underneath is untouched - the next restore() answers
    // the full layout again, and every introspector (leaf_count,
    // panels, leaf_of) keeps reading the whole tree.
    if (!maximized_.empty()) {
        const Node* zoomed = nullptr;
        const std::function<bool(const Node&)> find = [&](const Node& n) {
            if (!n.split) {
                for (const std::string& tab : n.tabs) {
                    if (tab == maximized_) {
                        zoomed = &n;
                        return true;
                    }
                }
                return false;
            }
            for (const std::unique_ptr<Node>& child : n.children) {
                if (find(*child)) {
                    return true;
                }
            }
            return false;
        };
        (void)find(*root_);
        if (zoomed != nullptr) {
            LeafRect leaf;
            leaf.leaf = 0;
            leaf.rect = bounds;
            leaf.tabs = &zoomed->tabs;
            leaf.active = zoomed->tabs.empty() ? 0 : zoomed->active;
            out.leaves.push_back(leaf);
            return out;  // no splitters: nothing drags while zoomed
        }
        // A zoom the tree no longer holds cannot happen through the
        // public seam (remove/clear/from_json drop it); falling
        // through to the full walk keeps a stale id from painting a
        // hole instead of a layout.
    }

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
    Node* node = node_at(splitter.path);
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

void DockTree::maximize(std::string_view panel) {
    if (!contains(panel)) {
        return;  // zooming a panel the tree does not hold is dead state
    }
    maximized_.assign(panel);
}

void DockTree::restore() { maximized_.clear(); }

void DockTree::distribute(const SplitterRect& splitter) {
    Node* node = node_at(splitter.path);
    if (node == nullptr || !node->split) {
        return;  // a stale band: the tree moved under it
    }
    // Even shares: equal weights, normalized to the split at resolve
    // time like every other weight.
    std::fill(node->weights.begin(), node->weights.end(), 1.0f);
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

int DockTree::tab_index(std::string_view panel) const {
    for (const Node* leaf : leaves_of(*root_)) {
        for (std::size_t i = 0; i < leaf->tabs.size(); ++i) {
            if (leaf->tabs[i] == panel) {
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
        const json::Object* wrap_obj = child.as_object();
        const json::Value* wrapped =
            wrap_obj == nullptr ? &child : wrap_obj->find("node");
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
        if (wrap_obj != nullptr) {
            const json::Value* w = wrap_obj->find("w");
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
    restore();  // a restored layout starts unzoomed (the zoom is
                // transient state, never part of the saved shape)
    // One panel, one tab: a hand-edited or merged layout may name an id
    // twice, and a doubled id is two leaves fighting over one instance.
    std::unordered_set<std::string> seen;
    const std::function<void(Node&)> dedupe = [&](Node& n) {
        if (!n.split) {
            std::vector<std::string> kept;
            kept.reserve(n.tabs.size());
            for (const std::string& tab : n.tabs) {
                if (seen.insert(tab).second) {
                    kept.push_back(tab);
                }
            }
            if (kept.size() != n.tabs.size()) {
                n.active = std::clamp(
                    n.active, 0,
                    std::max(0, static_cast<int>(kept.size()) - 1));
            }
            n.tabs = std::move(kept);
            return;
        }
        for (const std::unique_ptr<Node>& child : n.children) {
            dedupe(*child);
        }
    };
    dedupe(*root_);
    tidy();
    return true;
}

void DockTree::clear() {
    root_ = std::make_unique<Node>();
    restore();
}

}  // namespace ws::ui::shell
