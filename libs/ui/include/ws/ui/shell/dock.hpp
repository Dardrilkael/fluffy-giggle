#pragma once

// The dock host (PLAN_UI_STAGE14_15 stage 15): a split tree with tabbed
// leaves. Pure data + math, no widgets, no gui types beyond Rect: the
// tree answers WHERE things go, and the shell's view walks it to lay
// tab strips, panels and splitters. That split is what makes the
// serialization and the geometry testable headless, the same contract
// as every other headless half in the workspace.
//
// The full docking vocabulary lives here (the "future full Part A" the
// stage-15 notes reserved the seam for):
//   * add/move/move_tab - a panel enters the tree, re-docks at a new
//     placement, or reorders its tab; every placement is the same
//     five-zone grammar (center retabs, a side splits).
//   * fusion - a side placement whose target already sits in a split
//     of the same axis lands as ONE more sibling of that split, not a
//     fresh split nested inside the leaf. Trees stay flat, splitters
//     keep their meaning, and the pixel geometry is the one the
//     nested spelling would have drawn (the new leaf takes its share
//     of the target's span either way).
//   * maximize/restore - the focused-pane zoom: resolve() answers one
//     full-bounds leaf while a panel is zoomed.
//   * distribute - the splitter double-click: one split's children
//     even out.
//   * dock_hit - the five-zone drop walk over a resolved layout, the
//     drag-drop target picker the view (and the tests) share.
//
// Shape invariants (load() enforces, the mutators keep):
//   * the root always exists; a tree with no panels is one empty leaf.
//   * a split carries 2..N children, each with a weight >= kMinWeight;
//     weights are relative, normalized only at resolve() time.
//   * a leaf's tabs are unique; `active` is 0 for an empty leaf and
//     indexes the tab list otherwise.
//   * no empty splits: a child count that drops to 1 collapses into
//     its parent; leaves with no tabs are pruned (except the root).
//   * the maximized panel is transient state, never serialized: a
//     restored layout always starts unzoomed (to_json writes the same
//     tree zoomed or not).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ws/settings/json.hpp>
#include <ws/ui/gui/rect.hpp>

namespace ws::ui::shell {

// The gui's rect, imported under the shell's own name (the layout math
// speaks gui-space pixels; the import keeps the headers readable).
using Rect = ws::ui::gui::Rect;

// Which way a split divides its bounds (the splitter's axis).
enum class DockDir : std::uint8_t {
    horizontal = 0,  // children side by side, splitters drag left-right
    vertical = 1,    // children stacked, splitters drag up-down
};

// Where a new panel lands.
struct DockPlacement {
    enum class Area : std::uint8_t {
        center = 0,  // a new tab in the target leaf
        left = 1,    // a new leaf split off the target's left
        right = 2,
        top = 3,
        bottom = 4,
    };
    Area area = Area::center;
    int leaf = -1;  // target leaf index (resolve order); -1 = the last
    // The share of the split the NEW leaf takes, in (0,1) - clamped to
    // [kMinWeight, 1 - kMinWeight]. 0 (the default) = an even split,
    // the pre-weight behavior. The share is of the TARGET's own span:
    // a leaf that is itself 86% of the window, split with weight 0.28,
    // hands the new leaf ~24% of the window. Under fusion the share
    // means the same pixels: the target child's weight splits, the
    // siblings keep theirs.
    float weight = 0.0f;
    // Side placements only: split the WHOLE tree (the new leaf spans
    // the full width/height) instead of one leaf. How a bottom panel
    // gets the window's full width under every column. Ignored for
    // center, and on an empty tree (the panel just fills the root).
    // When the root is already a split of the matching axis the new
    // leaf fuses in as one more root child (a second bottom-strip
    // panel joins the strip instead of nesting a fresh split over it).
    bool at_root = false;
};

// The lower bound every weight is clamped to - a splitter cannot be
// dragged past 5% of its span, so no leaf ever disappears under its
// sibling.
inline constexpr float kMinWeight = 0.05f;

// The splitter thickness, in pixels, centered on the boundary between
// two children. The view draws and hit-tests this band.
inline constexpr float kSplitterPx = 6.0f;

class DockTree {
public:
    DockTree();

    DockTree(const DockTree&) = delete;
    DockTree& operator=(const DockTree&) = delete;
    DockTree(DockTree&&) noexcept;
    DockTree& operator=(DockTree&&) noexcept;
    ~DockTree();

    // --- panel membership ------------------------------------------------

    // Adds `panel` (a panel instance id). The target leaf is clamped
    // into range; `placement.area` center appends a tab and activates
    // it, any other area splits the target leaf - its old tabs keep one
    // side, the new leaf takes the other - and activates the new tab.
    // A side placement whose target sits in a split of the same axis
    // fuses: the new leaf becomes one more sibling of that split (see
    // the header's fusion note). Refuses an empty id or one already
    // docked (a no-op; a docked id re-docks through move()).
    void add(std::string_view panel, DockPlacement placement);

    // Re-docks `panel` at `placement`: the drag-drop primitive. Center
    // retabs into the target leaf (its front tab), a side splits the
    // target - the same fusion add() applies - and the panel's old
    // leaf prunes when it empties. A no-op for an unknown id; onto its
    // own leaf alone (nothing to split against - the tab just
    // activates); and center onto the leaf it already shares (the tab
    // activates; move_tab reorders within a strip).
    void move(std::string_view panel, DockPlacement placement);

    // Moves `panel`'s tab to `index` of leaf `leaf` (both clamped): a
    // reorder inside its own strip, or a retab into another strip (the
    // dropped tab becomes that leaf's front tab, and the emptied
    // source prunes). A no-op for an unknown id.
    void move_tab(std::string_view panel, int leaf, int index);

    // Removes one panel's tab wherever it lives, re-pointing `active`
    // and pruning the now-empty leaf (a split left with one child
    // collapses; the root stays). A no-op for an unknown id. Removing
    // the maximized panel restores the zoom (a stale zoom on a tree
    // that no longer holds the panel is dead state).
    void remove(std::string_view panel);

    // Makes `panel` the active tab of its leaf. A no-op when docked
    // nowhere; activating the already-active tab is the same no-op.
    void activate(std::string_view panel);

    [[nodiscard]] bool contains(std::string_view panel) const;

    // --- the layout walk ---------------------------------------------------

    // One resolved leaf: its index (the order resolve() lists leaves
    // in - depth-first, first child first), its bounds, and its tabs.
    struct LeafRect {
        int leaf = 0;
        Rect rect{};
        const std::vector<std::string>* tabs = nullptr;
        int active = 0;
    };

    // One resolved splitter: the drag band between two adjacent
    // children of one split, plus everything apply_drag() needs to move
    // it. `path` is the child indices root-to-split, `first_child` the
    // left/top child of the pair the splitter divides, and
    // span_start/span_size describe that pair's ALLOCATION inside the
    // split (pixels) so a dragged boundary maps back to weights.
    struct SplitterRect {
        std::vector<int> path;
        std::size_t first_child = 0;
        DockDir dir = DockDir::horizontal;
        Rect rect{};
        float span_start = 0.0f;
        float span_size = 0.0f;
    };

    // The resolved layout of `bounds`: every leaf's rect (tabs drawn by
    // the view) and every splitter band. Deterministic, allocation per
    // call - once per frame, the caller's shape. Weights normalize over
    // each split's children at resolve time only; the stored weights
    // never move.
    struct Layout {
        std::vector<LeafRect> leaves;
        std::vector<SplitterRect> splitters;
    };
    //
    // While a panel is maximized the answer is the zoom projection:
    // exactly one leaf (index 0) spanning the whole bounds with that
    // panel's leaf tabs and front tab, and no splitters (there is
    // nothing to drag while zoomed). The tree itself is untouched -
    // leaf_count()/panels()/leaf_of() keep answering the full tree, and
    // the zoom drops the moment the panel leaves it.
    [[nodiscard]] Layout resolve(const Rect& bounds) const;

    // Moves the boundary `splitter` (a resolved SplitterRect) to the
    // pixel `position` along its axis, clamped so both sides keep at
    // least kMinWeight of the pair's span. A position outside the span
    // clamps; the tree's other weights are untouched.
    void apply_drag(const SplitterRect& splitter, float position);

    // --- the focused-pane zoom ---------------------------------------------

    // resolve() answers only `panel`'s leaf (full bounds, no splitters)
    // until restore(). A no-op when `panel` is not docked; the zoom
    // follows the panel through move()/move_tab(), and clears itself
    // when the panel leaves the tree. Never serialized - a restored
    // layout starts unzoomed.
    void maximize(std::string_view panel);

    // Drops the zoom (a no-op when none). The tree was never touched:
    // the next resolve() answers the full layout again.
    void restore();

    // The zoomed panel's id, empty when the layout is not zoomed.
    [[nodiscard]] std::string_view maximized() const noexcept {
        return maximized_;
    }

    // --- splitter resets ---------------------------------------------------

    // Evens out the split `splitter` divides: every child of that split
    // resolves to the same share (the splitter's double-click). A no-op
    // for a stale band (a path the tree no longer holds).
    void distribute(const SplitterRect& splitter);

    // --- introspection -------------------------------------------------------

    // The leaf index `panel` is docked in, -1 when nowhere.
    [[nodiscard]] int leaf_of(std::string_view panel) const;

    // The index of `panel`'s tab inside its leaf, -1 when nowhere (the
    // tab-strip coordinate move_tab speaks).
    [[nodiscard]] int tab_index(std::string_view panel) const;

    // The number of leaves, resolve order (>= 1: the empty root leaf).
    // The full tree, zoom or not - resolve()'s projection is a view,
    // not the tree.
    [[nodiscard]] int leaf_count() const;

    // Every docked panel id, depth-first in leaf order, tab order.
    [[nodiscard]] std::vector<std::string> panels() const;

    // --- persistence -------------------------------------------------------

    // The json shape (a whole tree):
    //   {"tabs": ["a","b"], "active": 0}                       a leaf
    //   {"dir": "h", "children": [{"w": 1, "node": {...}}, ...]}  a split
    // Weights serialize normalized to 1 over the split. Deterministic
    // (fixed key order, shortest-round-trip reals - the settings
    // writer's own guarantees). The zoom is never written: the shape
    // is the same tree zoomed or not.
    [[nodiscard]] ws::settings::json::Value to_json() const;

    // Parses that shape back. Tolerant by contract: a subtree that is
    // not the shape it should be is dropped (its parent collapses when
    // it runs short of children); a tab `accept` refuses is skipped (a
    // panel that no longer exists must not resurrect); a leaf that
    // keeps no tabs is dropped the same way. Nothing throws - the
    // worst hand-edit lands on the default single-leaf tree, and the
    // bool answers whether ANY tree came out (false only for a
    // non-object root). Any zoom clears: a restored layout starts
    // unzoomed.
    [[nodiscard]] bool from_json(const ws::settings::json::Value& value,
                                 const std::function<bool(
                                     std::string_view panel)>& accept);

    // Rebuilds the default: one empty leaf (the caller re-adds panels
    // at their registered default areas). Drops any zoom.
    void clear();

private:
    struct Node;

    // The leaf walks, static members so the .cpp's recursions can name
    // the private Node type (a free function cannot).
    static void collect_leaves(const Node& node,
                               std::vector<const Node*>& out);
    [[nodiscard]] static std::vector<const Node*> leaves_of(
        const Node& root);
    [[nodiscard]] static Node* leaf_at_index(Node& root, int index);
    // Prunes empty leaves and collapses single-child splits, one level.
    static void prune(Node& node);
    // prune() plus the root's own edge cases: a split that lost every
    // child is an empty root leaf again, and an empty root leaf's
    // active is 0. The tail remove()/from_json()/move() share.
    void tidy();

    // The mutation seams. erase_tab takes a tab out of a leaf keeping
    // `active` pointing at the same tab; insert_leaf installs `fresh`
    // (a one-tab leaf) at `placement` relative to `target` - center or
    // an empty target fills, a side fuses into the matching parent
    // split or wraps the target - and answers the leaf node the payload
    // landed in; wrap is the split-a-node-in-two half both paths end
    // in (the node OBJECT is reused as the split, so the pointers
    // above it - root_, a parent's children - stay valid). parent_of
    // names a node's parent (and its child index) for the fusion walk;
    // node_at walks a splitter's path.
    static void erase_tab(Node& leaf, std::size_t tab);
    [[nodiscard]] static Node* insert_leaf(Node& root, Node& target,
                                           DockPlacement placement,
                                           std::unique_ptr<Node> fresh);
    [[nodiscard]] static Node* wrap(Node& node, DockDir dir, bool before,
                                    float share,
                                    std::unique_ptr<Node> fresh);
    [[nodiscard]] static Node* parent_of(Node& node, Node& child,
                                         int* index);
    [[nodiscard]] Node* node_at(const std::vector<int>& path);

    // The json halves (to_json/from_json's recursion).
    [[nodiscard]] static ws::settings::json::Value node_to_json(
        const Node& node);
    [[nodiscard]] static std::unique_ptr<Node> node_from_json(
        const ws::settings::json::Value& value,
        const std::function<bool(std::string_view)>& accept);

    std::unique_ptr<Node> root_;
    std::string maximized_;  // "" = the layout is not zoomed
};

// The five-zone drop walk: which leaf of a resolved layout the point
// lands in, and the placement a drop there implies - center, or the
// edge within `band` pixels (the band clamps to a quarter of the
// leaf's span per side, so a center zone of at least half the leaf
// always exists - a band wider than that would leave a leaf with
// nothing but edges). The x edges outrank the y edges (a corner
// answers the side), the first leaf containing the point wins (leaves
// never overlap), and a point outside every leaf answers leaf -1: no
// drop. The picker the shell's tab drag and the headless tests share
// - pure walk, no tree mutation.
struct DockHit {
    int leaf = -1;
    DockPlacement::Area area = DockPlacement::Area::center;
};
[[nodiscard]] inline DockHit dock_hit(const DockTree::Layout& layout,
                                      float x, float y,
                                      float band = 32.0f) {
    for (const DockTree::LeafRect& leaf : layout.leaves) {
        const Rect& r = leaf.rect;
        const float bx = std::min(band, r.width * 0.25f);
        const float by = std::min(band, r.height * 0.25f);
        if (x < r.x || x >= r.x + r.width || y < r.y ||
            y >= r.y + r.height) {
            continue;
        }
        if (x < r.x + bx) {
            return {leaf.leaf, DockPlacement::Area::left};
        }
        if (x >= r.x + r.width - bx) {
            return {leaf.leaf, DockPlacement::Area::right};
        }
        if (y < r.y + by) {
            return {leaf.leaf, DockPlacement::Area::top};
        }
        if (y >= r.y + r.height - by) {
            return {leaf.leaf, DockPlacement::Area::bottom};
        }
        return {leaf.leaf, DockPlacement::Area::center};
    }
    return {};
}

}  // namespace ws::ui::shell
