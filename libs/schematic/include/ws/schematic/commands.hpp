#pragma once

// ws::sch - the move command + undo stack (phase 3). The lib owns the
// payload and the exact-restore machinery; the editor app owns the
// stack's seat in its own undo system (the adapter forwards). Everything
// here remains headless: no ImGui, no ui, no rendering.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <ws/schematic/model.hpp>

namespace ws::sch {

// One connect-on-drop that actually landed in a commit. Public plain data
// (the stack's private Entry holds a list of them): redo replays a step
// with the ids it minted, undo erases exactly those ids.
struct LandedStep {
    enum class Kind : std::uint8_t {
        Split,   // cut `wire` at `at` -> junction + second half + tie
        Join,    // tie `pin` to the existing junction `other`
        PinPin,  // a wire between pins `pin` and `other`
    };
    Kind kind = Kind::Split;
    WireId wire = 0;      // Split: the wire that was cut
    Vec2 at{0.0, 0.0};    // Split: where
    NodeId pin = 0;       // the dragged pin
    NodeId other = 0;     // Join / PinPin: the node it was tied to
    NodeId junction = 0;  // Split: minted junction
    WireId second = 0;    // Split: minted b-side half
    WireId tie = 0;       // minted tie wire (all kinds)
};

// The undo stack the lib's own tests drive. The editor app either uses
// this directly (the adapter can own one per document) or mirrors the
// commands into its own stack - the payload types are plain data either
// way.
class UndoStack {
public:
    // ONE command per gesture: the overlay (positions + wire shapes) and
    // the connect-on-drop edits (splits minted, wires added). Undo is
    // the exact inverse.
    bool undo(Doc& d);
    bool redo(Doc& d);
    [[nodiscard]] bool undoable() const { return at_ > 0u; }
    [[nodiscard]] bool redoable() const { return at_ < list_.size(); }
    [[nodiscard]] std::size_t size() const { return list_.size(); }
    [[nodiscard]] bool empty() const { return list_.empty(); }
    [[nodiscard]] std::string name(std::size_t i) const {
        return i < list_.size() ? list_[i].name : std::string();
    }

    void clear() {
        list_.clear();
        at_ = 0;
    }

    // The stack is moved with the document (tests carry it beside the
    // Doc; the app adapter embeds it).
    [[nodiscard]] std::size_t at() const { return at_; }

private:
    friend bool commit_move(Doc&, UndoStack&, const Overlay&,
                            const std::vector<DropCandidate>&);

    // ONE command's full state: the applied overlay (redo), the exact
    // before-state of everything touched (undo), and one STEP per
    // connect-on-drop that actually landed. Redo replays the steps with
    // the ids they minted, undo erases exactly what they minted - so a
    // candidate that could not land leaves no trace and nothing is
    // positional (no cursor to misalign).
    struct Entry {
        std::string name;
        Overlay overlay;
        std::vector<std::pair<NodeId, Vec2>> node_before;
        std::vector<std::pair<WireId, Wire>> wire_before;
        std::vector<LandedStep> steps;
    };
    // Entries kept; the oldest is dropped past this (undo memory is
    // bounded: each entry holds full copies of the wires it touched).
    static constexpr std::size_t kMaxEntries = 1000;

    // Drops the redo tail, appends, trims to kMaxEntries.
    void push_entry(Entry e);

    std::vector<Entry> list_;
    std::size_t at_ = 0;
};

// Commits a drag release: applies the overlay (node positions, wire
// shapes, end dirs) AND exactly the candidates the drag showed (each
// PinOnWire lands as split_wire at its point; each PinOnPin lands as a
// wire between the pins), as ONE undo command named "Move". An empty
// overlay with no candidates pushes nothing (a no-op gesture is no step).
// Returns true when a command was pushed.
bool commit_move(Doc& d, UndoStack& undo, const Overlay& overlay,
                 const std::vector<DropCandidate>& candidates);

}  // namespace ws::sch
