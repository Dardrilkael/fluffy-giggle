#pragma once

// ws::schematic - the explicit-connectivity wire/move core, the rewrite
// docs/schematic-current.md specifies. THIS library owns the schematic
// CONNECTIVITY MODEL: a node graph (pins, junctions, labels) plus wires
// over node ids, and nets as pure union-find over that graph.
//
// The rules, non-negotiable (the whole reason this lib exists):
//   * GEOMETRY NEVER CREATES CONNECTIVITY. A wire crossing or touching
//     another wire, a wire's path passing over a pin or a junction dot
//     - none of it connects anything. The drawing shows geometry; the
//     graph alone decides what is connected.
//   * A T-CONNECTION IS A JUNCTION NODE THAT SPLITS THE WIRE. There are
//     no "interior attachments" anywhere: no wire keeps a list of dots
//     pinned to its body, no dot keeps a list of wires it taps. A branch
//     drawn onto a wire's body becomes a junction node at that point with
//     the original wire SPLIT into two wires meeting there.
//   * THE OVERLAY IS THE ONE ROUTING CODE PATH. resolve_move(Doc,
//     MoveSession) answers an Overlay - pure data, no mutation, no ui
//     types. The editor's drag PREVIEW draws doc + overlay; the release
//     applies THE SAME OVERLAY as one undo command. Live preview and
//     commit cannot drift because they are the same value.
//
// Everything here is headless C++20 (ws::core's Result/Assert, ws::math's
// DVec2) - no ImGui, no rendering, no editor types; fully unit-testable.
// The editor app adapts its own document to Doc at the seam (editor's
// SchematicAdapter) and owns all input/undo policy.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ws/math/vecmath.hpp>

namespace ws::sch {

using Vec2 = ws::math::DVec2;

// A wire id / node id / label id. Ids are minted by Doc and NEVER reused
// within a document (the .csim carrier rule): 0 is the "none" sentinel.
using NodeId = std::uint32_t;
using WireId = std::uint32_t;
using LabelId = std::uint32_t;
using PartId = std::uint32_t;

// The four cardinal exit directions, degrees CCW in the model frame
// (y up), matching the symbol library's pin dirs. -1 means "free"
// (no committed lead-out; junctions and free wire ends).
inline constexpr int kDirFree = -1;

// --- nodes -----------------------------------------------------------------

enum class NodeKind : std::uint8_t { Pin, Junction, Label };

// A node in the connectivity graph. `pos` is the node's world position
// (grid units, y up) - DRAWING data only: it never affects connectivity,
// it is what the router shapes wires around and what the editor snaps.
struct Node {
    NodeId id = 0;
    NodeKind kind = NodeKind::Junction;
    // Pin nodes: the owning part + the symbol pin index. Non-pin nodes
    // leave these 0.
    PartId part = 0;
    std::uint32_t pin = 0;
    Vec2 pos{0.0, 0.0};

    [[nodiscard]] bool is_pin() const { return kind == NodeKind::Pin; }

    bool operator==(const Node&) const = default;

    // A pin node's exit direction is resolved by the ADAPTER from the
    // part's placement (symbol lib), not stored here: the lib owns the
    // graph, the app owns the symbol geometry. Junctions/labels answer
    // kDirFree.
};

// --- wires ------------------------------------------------------------------

// A wire: two node endpoints plus interior orthogonal waypoints.
// The endpoints are NODE IDS (pins, junctions, labels) - connectivity is
// exactly "these two nodes are joined by a conductor", nothing inferred.
struct Wire {
    WireId id = 0;
    NodeId a = 0;
    NodeId b = 0;
    // Interior corners (neither endpoint). Stored exactly as drawn after
    // the router's cleanup pass; orthogonal by construction.
    std::vector<Vec2> mid;
    // The exit direction each end was drawn with (-1 = free). PART of the
    // wire's authored shape: a re-route honors it at the FIXED end; a move
    // keeps it when the end follows its part. This is what makes "the wire
    // leaves the pin east" a stored fact instead of a re-derived guess.
    int a_dir = kDirFree;
    int b_dir = kDirFree;
    // The pre-split parent, for the migration: a wire that used to carry a
    // T-tap on its interior and was split into (W1, junction, W2) marks
    // both halves so a round trip can re-join them. 0 = no parent.
    WireId split_of = 0;

    bool operator==(const Wire&) const = default;
};

// A label: names a net. A label NODE joins the graph when its position
// coincides with a net's attachment point (the adapter's authored
// binding); same-name labels union.
struct Label {
    LabelId id = 0;
    Vec2 pos{0.0, 0.0};
    std::string name;
    // The authored binding (the .csim `wire` field): the wire this label
    // was placed on; 0 = unbound (legacy), attaches by the adapter's rules.
    WireId wire = 0;

    bool operator==(const Label&) const = default;
};

// --- the document -----------------------------------------------------------

// Ids to use for the entities split_wire creates. 0 = mint a fresh one.
// A redo replays a committed split with the ids the commit minted, so the
// replay is the SAME function with the same ids (no second implementation
// to drift from the first).
struct SplitIds {
    NodeId junction = 0;
    WireId second = 0;
};

class Doc {
public:
    // Mints a fresh id of each kind (never reused: next_* only rise).
    NodeId alloc_node() { return ++next_node_; }
    WireId alloc_wire() { return ++next_wire_; }
    LabelId alloc_label() { return ++next_label_; }

    // An undo's wholesale restore re-inserts entities with their own ids;
    // the counters must stay AHEAD of every live id or the next alloc
    // would mint a duplicate.
    void raise_wire_counter(WireId w) {
        if (w >= next_wire_) {
            next_wire_ = w + 1;
        }
    }
    void raise_node_counter(NodeId n) {
        if (n >= next_node_) {
            next_node_ = n + 1;
        }
    }
    void raise_label_counter(LabelId l) {
        if (l >= next_label_) {
            next_label_ = l + 1;
        }
    }

    // --- storage ---
    // The lib's own truth. The editor ADAPTS its ws::circsim Schematic
    // into this view at the seam and never mutates storage directly
    // except through the seam's re-sync.
    std::vector<Node> nodes;
    std::vector<Wire> wires;
    std::vector<Label> labels;

    [[nodiscard]] const Node* find_node(NodeId id) const;
    [[nodiscard]] Node* find_node(NodeId id);
    [[nodiscard]] const Wire* find_wire(WireId id) const;
    [[nodiscard]] Wire* find_wire(WireId id);
    [[nodiscard]] const Label* find_label(LabelId id) const;

    // Simple builders (tests, the adapter, the loader).
    NodeId add_pin(PartId part, std::uint32_t pin, Vec2 pos);
    NodeId add_junction(Vec2 pos);
    LabelId add_label(Vec2 pos, std::string name, WireId wire = 0);
    WireId add_wire(NodeId a, NodeId b, std::vector<Vec2> mid = {},
                    int a_dir = kDirFree, int b_dir = kDirFree);

    // Re-inserts an entity with its own id (undo / redo / the loader) and
    // keeps the counters ahead of it.
    WireId restore_wire(const Wire& w);

    // THE STRUCTURAL EDIT: a T-connection is made by SPLITTING. Inserts a
    // junction node at `pos` ON wire `w` (an off-path pos, or a pos at the
    // wire's own end, is refused with nullopt) and cuts the wire into two
    // halves meeting at the junction - the branch then lands its own wire
    // on the junction node. There are no interior attachments anywhere:
    // the split IS the connection.
    //
    // IDENTITY: the a-side half KEEPS the original wire's id, so anything
    // that names the wire by id (labels' authored binding, undo records,
    // the editor's selection) stays valid; only the b-side half and the
    // junction are new.
    // `split_of` marks every piece with one group id (migration + re-join).
    struct Split {
        NodeId junction = 0;
        WireId first = 0;   // the a-side half (== the original wire id)
        WireId second = 0;  // the b-side half
    };
    std::optional<Split> split_wire(WireId w, Vec2 pos);
    std::optional<Split> split_wire(WireId w, Vec2 pos, SplitIds ids);

    // The inverse: if exactly two wires meet at junction `j` and both were
    // split from the same parent (or the caller passes allow_any=true for
    // a plain two-wire dot), remove the junction and merge the halves back
    // into one wire. The merged wire KEEPS the a-side half's id and labels
    // bound to the other half are re-bound to it. Returns the merged wire
    // id, 0 when refused.
    WireId merge_split(NodeId j, bool allow_any = false);

    // Removes a node (does not touch wires that still name it).
    bool erase_node(NodeId n);

    // Removes a wire (leaves its endpoint nodes alone - a junction that
    // loses its last wire stays as a dot the editor may clean up by
    // policy). Returns false when the id is unknown.
    bool erase_wire(WireId w);

    // The resolved world polyline of a wire: a's node pos, the mid
    // waypoints, b's node pos. A missing endpoint node answers EMPTY
    // (a dangling wire has nothing to draw or route).
    [[nodiscard]] std::vector<Vec2> wire_path(WireId w) const;

private:
    NodeId next_node_ = 0;
    WireId next_wire_ = 0;
    LabelId next_label_ = 0;
};

// --- nets -------------------------------------------------------------------

// A net: the connected component of the node graph. Pure union-find over
// nodes + wires (+ labels through their authored binding): geometry never
// enters. Answered as sorted node-id sets per net, so "the set of pin-to-
// pin connections" is stable and comparable across edits.
struct NetList {
    // Each net: every node id in the component (pins, junctions) plus a
    // label marker (see label_marker) per bound label that names it,
    // sorted. Net order is by first id (stable).
    std::vector<std::vector<NodeId>> nets;

    // The net containing node `n`, or nullptr.
    [[nodiscard]] const std::vector<NodeId>* net_of(NodeId n) const;
};

// A BOUND label appears in a net as a marker id, not a node id: the high
// bit plus the label's index in Doc::labels. Node ids are minted from 1
// upward and must stay below kLabelMarkerBit (2^31) - far beyond any
// real document. Use these helpers; never open-code the bit.
inline constexpr NodeId kLabelMarkerBit = 0x8000'0000u;
[[nodiscard]] constexpr NodeId label_marker(std::size_t label_index) {
    return kLabelMarkerBit | static_cast<NodeId>(label_index);
}
[[nodiscard]] constexpr bool is_label_marker(NodeId id) {
    return (id & kLabelMarkerBit) != 0u;
}
[[nodiscard]] constexpr std::size_t label_index(NodeId id) {
    return static_cast<std::size_t>(id & ~kLabelMarkerBit);
}

// Builds the nets: DSU over (nodes joined by wires) + (labels joined by
// the authored wire binding + same-name merging). GEOMETRY NEVER CREATES
// CONNECTIVITY - crossing or touching wires do not connect, and a node
// merely lying on some wire's path connects nothing (the drawing's
// business, warned by the app's ERC, decided here: not connected).
[[nodiscard]] NetList build_nets(const Doc& doc);

// The set of distinct pin-to-pin connections: every unordered pair of
// PIN nodes sharing a net (the invariant the property test rides - a
// move must change this set ONLY through an explicit connect-on-drop).
using PinPair = std::pair<NodeId, NodeId>;
[[nodiscard]] std::vector<PinPair> pin_connections(const Doc& doc);

// --- the move session + overlay (phase 2) ----------------------------------

// What is being dragged. The session is PURE INPUT DATA the canvas
// builds; every topology decision stays in this lib.
struct MoveSession {
    // The parts being moved (the adapter maps part ids; a junction drag
    // is "one node, no parts"). Each part's pin nodes follow by the
    // resolved delta - the adapter hands their CURRENT positions in the
    // session, so the lib needs no symbol knowledge.
    struct PartMove {
        PartId part = 0;
        Vec2 to{0.0, 0.0};
    };
    std::vector<PartMove> parts;
    // Free nodes dragged alone (junctions, labels; a wire's loose end).
    std::vector<NodeId> nodes;
    // Where each dragged node lands (parts' pins derive from their part;
    // the session carries the ADAPTER-RESOLVED landing for every dragged
    // node id - one map, no symbol math inside the lib).
    std::map<NodeId, Vec2> land;
    // Wire endpoints dragged to a NEW FREE POSITION (the wire-end drag:
    // the end leaves its node and lands at a point - the COMMIT mints a
    // junction node there / re-binds; the OVERLAY only routes).
    struct EndMove {
        WireId wire = 0;
        bool a_side = false;  // true: the a end, false: the b end
        Vec2 to{0.0, 0.0};
    };
    std::vector<EndMove> ends;
    // A rotation flips pin exit dirs; the adapter passes each affected
    // pin node's NEW exit direction so the router honors the landing.
    std::map<NodeId, int> land_dir;
};

// The pure answer to a session: the world positions and wire shapes the
// document WOULD have - computed, never applied. The preview draws
// doc + overlay; the commit applies the SAME value as one undo command.
struct Overlay {
    // Moved node positions (the session's nodes + every part pin node
    // that landed somewhere new).
    std::map<NodeId, Vec2> nodePos;
    // Fresh interior waypoints for every wire the session bent (a wire
    // not present here keeps its stored shape).
    std::map<WireId, std::vector<Vec2>> wirePath;
    // Fresh exit directions for ends whose pin turned (rotations).
    std::map<std::pair<WireId, bool>, int> endDir;

    [[nodiscard]] bool empty() const {
        return nodePos.empty() && wirePath.empty() && endDir.empty();
    }
};

// Resolve a move session into the overlay. Pure: reads doc + session,
// answers positions + wire shapes, touches nothing. The router rules
// (router.cpp): both ends moved -> rigid translate; one end moved ->
// keep the fixed end's stub + pin exit direction, slide the nearest
// waypoint, cheapest L/Z/U pattern (bends, then length), deterministic,
// no A*; final cleanup removes collinear and zero-length segments.
[[nodiscard]] Overlay resolve_move(const Doc& doc,
                                    const MoveSession& session);

// --- connect-on-drop (phase 3) ----------------------------------------------

// A CANDIDATE connection the drag would make on release. Computed during
// the drag and EXPOSED for snap-ring rendering; the commit creates ONLY
// the candidates that were shown (and still valid) at release.
struct DropCandidate {
    enum class Kind : std::uint8_t {
        PinOnWire,  // the dragged pin lands on a wire's path -> SPLIT
        PinOnPin,   // the dragged pin lands on another pin -> a tiny wire
    };
    Kind kind = Kind::PinOnWire;
    NodeId pin = 0;    // the dragged pin node
    WireId wire = 0;   // PinOnWire: the wire whose path carries the pin
    Vec2 at{0.0, 0.0}; // PinOnWire: the split point; PinOnPin: shared pos
    NodeId other = 0;  // PinOnPin: the pin already there
};

// The candidates a drag's LANDING offers (pin-on-wire: the pin's overlay
// position lies on another wire's resolved path; pin-on-pin: it
// coincides with another pin node's position). Wires moved over other
// wires NEVER auto-connect - only pins offer candidates.
// Reads doc + overlay (the landing's positions), answers pure data.
[[nodiscard]] std::vector<DropCandidate> drop_candidates(
    const Doc& doc, const Overlay& overlay);

// --- commands (phase 3) -----------------------------------------------------

// The ONE undo command a drag release lands: the overlay (positions +
// wire shapes) plus the candidates the drag showed and the release
// confirmed. apply() = the commit; undo() = the exact inverse. Declared
// here, implemented over the app's undo stack in commands.cpp - the lib
// owns the payload type, the app owns the stack.
struct MoveCommand {
    Overlay overlay;
    std::vector<DropCandidate> candidates;
    // apply()'s bookkeeping (split halves minted, wires re-bound) rides
    // the app's command; the payload is plain data.
};

}  // namespace ws::sch
