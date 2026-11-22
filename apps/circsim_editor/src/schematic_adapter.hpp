#pragma once

// apps/circsim_editor - the schematic adapter: the bridge between the
// app's persisted document (ws::circsim::Schematic, the .csim carrier)
// and the new explicit-connectivity core (ws::sch::Doc). This is the
// ONLY translation layer: everything else in the app speaks one side.
//
// THE DESIGN (docs/schematic-current.md's rewrite, phase 4):
//   * the PERSISTED document stays ws::circsim::Schematic (ids, format,
//     ERC, export, net coloring all keep their seams);
//   * the MOVE/ROUTE/CONNECT decisions all ride ws::sch: the adapter
//     builds a sch::Doc VIEW over the schematic (a pure, per-gesture
//     rebuild - it is small and the drag is the only reader), the canvas
//     turns input into a sch::MoveSession, resolve_move answers the
//     Overlay, the canvas DRAWS doc+overlay, and the release lands ONE
//     undo command whose payload IS the overlay (plus the connect-on-
//     drop candidates the drag showed). There is exactly ONE routing
//     code path - the lib's.
//   * the adapter also carries the overlay BACK to circsim terms (the
//     part positions and wire waypoints to write) so the command layer
//     needs no ws::sch knowledge beyond this file.
//
// SAVE/LOAD MIGRATION (phase 4d): the PERSISTED format is unchanged -
// the .csim keeps junctions with their authored `taps` lists, so old
// files load exactly as before and new saves stay readable by old
// builds. The lift is LIVE: every build() turns each tap into a SPLIT
// of the tapped wire at the junction's position (the T-connection IS
// the split in the new core), and landing() merges the split pair back
// into ONE persisted wire's waypoint list with the junction's position
// between the halves - so a drag that re-routes the halves saves as the
// same single wire, tap still on its path, still old-format.
//
// Headless: no ui include; the canvas and the tests both drive it.

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/schematic/commands.hpp>
#include <ws/schematic/model.hpp>

namespace editor {

using ws::sch::NodeId;
using ws::sch::Overlay;

// The view: a sch::Doc built from the schematic + library, plus the
// back-maps (sch node id -> the wire/part-pin/junction it stands for).
// Rebuilt per gesture (cheap: N entities, straight copy).
struct SchematicAdapter {
    ws::sch::Doc doc;
    // Pin nodes: (part id, pin index) -> node id.
    std::map<std::pair<std::uint32_t, std::uint32_t>, NodeId> pin_node;
    // Junction nodes: junction id -> node id.
    std::map<std::uint32_t, NodeId> junction_node;
    // Wires: circsim wire id -> sch wire id.
    std::map<std::uint32_t, ws::sch::WireId> wire_ids;
    // Node -> the circsim junction it replaced (0 = none).
    std::map<NodeId, std::uint32_t> node_junction;
    // sch wire id -> circsim wire id (inverse of wire_ids).
    std::map<ws::sch::WireId, std::uint32_t> wire_back;
    // The persisted schematic it mirrors (not owned).
    ws::circsim::Schematic* schem = nullptr;
    const ws::circsim::SymbolLibrary* lib = nullptr;

    // Builds the view. Taps: the persisted taps become SPLITS in the
    // view (the T-connection IS the split) - the migration's own rule,
    // applied live so the graph the router sees is already the new one.
    //
    // `press` (optional) re-poses parts and junctions for the build
    // WITHOUT touching the schematic: a drag builds the view at the
    // PRESS geometry (the router bends the corridors the press
    // authored) while the schematic keeps the live positions the
    // canvas draws. Entities not named keep their stored position.
    struct PressPose {
        std::map<std::uint32_t, ws::circsim::Pos> parts;      // part id
        std::map<std::uint32_t, ws::circsim::Pos> junctions;  // junction id
    };
    void build(ws::circsim::Schematic& s,
               const ws::circsim::SymbolLibrary& library,
               const PressPose* press = nullptr);

    // --- session builders (the canvas turns input into these) --------

    // A part drag: the part's pin nodes land at their resolved positions
    // (the adapter owns the symbol math), each pin's stored world exit
    // dir rides unchanged (the pin's face turned with the part).
    [[nodiscard]] ws::sch::MoveSession part_session(
        std::uint32_t part_id, ws::circsim::Pos to) const;

    // A rotation: the part stays at its position but its pins face new
    // directions (land_dir entries; the pin node ids are stable).
    [[nodiscard]] ws::sch::MoveSession rotate_session(
        std::uint32_t part_id, int rot, bool mirror) const;

    // A junction drag: the node lands at `to`.
    [[nodiscard]] ws::sch::MoveSession junction_session(
        std::uint32_t junction_id, ws::circsim::Pos to) const;

    // A group drag: every member's landing (kind as the app's
    // Selection::Kind values: 1 = Part, 3 = Junction - the editor's
    // own enum spelled plainly here so the adapter holds no editor.hpp
    // include).
    struct MemberMove {
        std::uint32_t kind = 0;  // 1 = part, 3 = junction
        std::uint32_t id = 0;
        ws::circsim::Pos to{0.0, 0.0};
    };
    [[nodiscard]] ws::sch::MoveSession group_session(
        const std::vector<MemberMove>& members) const;

    // --- the overlay back to circsim ----------------------------------

    // What the release must write into the PERSISTED document: the
    // moved junctions and every wire's fresh waypoints, all in
    // circsim's own ids. (Part positions are the drag's own `to` -
    // the canvas passes them; a pin node's landing is derivable only
    // through the symbol math the adapter already ran for the session.)
    // This is the ONE routing answer's translation; the command layer
    // never re-derives anything.
    struct Landing {
        // junction id -> position.
        std::map<std::uint32_t, ws::circsim::Pos> junctions;
        // circsim wire id -> fresh interior waypoints.
        std::map<std::uint32_t, std::vector<ws::circsim::Pos>> wires;
    };
    [[nodiscard]] Landing landing(const Overlay& ov) const;

    // The connect-on-drop candidates at the LANDING (what the snap rings
    // draw and only what the commit may create), translated to
    // circsim's terms.
    struct Drop {
        enum class Kind : std::uint8_t { PinOnWire, PinOnPin };
        Kind kind = Kind::PinOnWire;
        std::pair<std::uint32_t, std::uint32_t> pin{};
        std::uint32_t wire = 0;      // PinOnWire: the circsim wire id
        ws::circsim::Pos at{};
        std::pair<std::uint32_t, std::uint32_t> other{};
    };
    [[nodiscard]] std::vector<Drop> drops(const Overlay& ov) const;
};

}  // namespace editor
