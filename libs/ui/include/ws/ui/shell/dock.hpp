#pragma once

// The dock host (PLAN_UI_STAGE14_15 stage 15): a split tree with tabbed
// leaves, the minimal in-repo version of Part A's docking - the seam is
// clear so a future full Part A can replace it. Pure data + math, no
// widgets, no gui types: the tree answers WHERE things go, and the
// shell's view walks it to lay tab strips, panels and splitters. That
// split is what makes the serialization and the geometry testable
// headless, the same contract as every other headless half in the
// workspace.
//
// Shape invariants (load() enforces, the mutators keep):
//   * the root always exists; a tree with no panels is one empty leaf.
//   * a split carries 2..N children, each with a weight >= kMinWeight;
//     weights are relative, normalized only at resolve() time.
//   * a leaf's tabs are unique; `active` is 0 for an empty leaf and
//     indexes the tab list otherwise.
//   * no empty splits: a child count that drops to 1 collapses into
//     its parent; leaves with no tabs are pruned (except the root).

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
    // Refuses an empty id or one already docked (invalid_argument).
    void add(std::string_view panel, DockPlacement placement);

    // Removes one panel's tab wherever it lives, re-pointing `active`
    // and pruning the now-empty leaf (a split left with one child
    // collapses; the root stays). A no-op for an unknown id.
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
    [[nodiscard]] Layout resolve(const Rect& bounds) const;

    // Moves the boundary `splitter` (a resolved SplitterRect) to the
    // pixel `position` along its axis, clamped so both sides keep at
    // least kMinWeight of the pair's span. A position outside the span
    // clamps; the tree's other weights are untouched.
    void apply_drag(const SplitterRect& splitter, float position);

    // --- introspection -------------------------------------------------------

    // The leaf index `panel` is docked in, -1 when nowhere.
    [[nodiscard]] int leaf_of(std::string_view panel) const;

    // The number of leaves, resolve order (>= 1: the empty root leaf).
    [[nodiscard]] int leaf_count() const;

    // Every docked panel id, depth-first in leaf order, tab order.
    [[nodiscard]] std::vector<std::string> panels() const;

    // --- persistence -------------------------------------------------------

    // The json shape (a whole tree):
    //   {"tabs": ["a","b"], "active": 0}                       a leaf
    //   {"dir": "h", "children": [{"w": 1, "node": {...}}, ...]}  a split
    // Weights serialize normalized to 1 over the split. Deterministic
    // (fixed key order, shortest-round-trip reals - the settings
    // writer's own guarantees).
    [[nodiscard]] ws::settings::json::Value to_json() const;

    // Parses that shape back. Tolerant by contract: a subtree that is
    // not the shape it should be is dropped (its parent collapses when
    // it runs short of children); a tab `accept` refuses is skipped (a
    // panel that no longer exists must not resurrect); a leaf that
    // keeps no tabs is dropped the same way. Nothing throws - the
    // worst hand-edit lands on the default single-leaf tree, and the
    // bool answers whether ANY tree came out (false only for a
    // non-object root).
    [[nodiscard]] bool from_json(const ws::settings::json::Value& value,
                                 const std::function<bool(
                                     std::string_view panel)>& accept);

    // Rebuilds the default: one empty leaf (the caller re-adds panels
    // at their registered default areas).
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

    // The json halves (to_json/from_json's recursion).
    [[nodiscard]] static ws::settings::json::Value node_to_json(
        const Node& node);
    [[nodiscard]] static std::unique_ptr<Node> node_from_json(
        const ws::settings::json::Value& value,
        const std::function<bool(std::string_view)>& accept);

    std::unique_ptr<Node> root_;
};

}  // namespace ws::ui::shell
