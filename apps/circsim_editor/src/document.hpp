#pragma once

// apps/circsim_editor stage 3 - the document layer: the .csim
// shell::Document plus the whole undo command set the editor edits
// through. The rules that shape every command here:
//
//   * PUSH IS THE EDIT (ws::app::UndoStack's contract): apply() runs
//     on push, so a caller hands the command the FINAL state and
//     never pre-applies anything itself.
//   * Ids are never reused (the model's rule): a command that
//     re-inserts an entity keeps its id and re-raises next_id - the
//     add_* API mints fresh ids, so re-insertion goes through the
//     *_mut() containers the way the loader does, with bump() after.
//   * Deleting a part takes its wires with it (a dangling PinRef is
//     the loader's tolerance, never something the editor creates);
//     removing wires takes junctions nothing else references (an
//     orphaned junction is ERC's "empty net", not a keepsake).
//
// SchematicDocument is otherwise the thin .csim half: load/save
// through the settings FileSystem seam (atomic writes by contract),
// autosave content = the canonical serialization, and the load report
// kept for the Problems panel to show.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ws/app/undo.hpp>
#include <ws/circsim/format.hpp>
#include <ws/circsim/geom.hpp>  // WireRoute + next_ref (stage 11 - the lib
                                 // extraction; the aliases below re-export)
#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/ui/shell/document.hpp>

namespace editor {

class SchematicDocument final : public ws::ui::shell::Document {
  public:
    // `fs` must outlive the document (the load/save seam).
    SchematicDocument(std::string path, ws::settings::FileSystem& fs);

    [[nodiscard]] const std::string& path() const override;
    void set_path(std::string path) override;
    [[nodiscard]] std::string display_name() const override;
    [[nodiscard]] std::string doc_type() const override;

    [[nodiscard]] ws::core::Status load() override;
    [[nodiscard]] ws::core::Status save() override;

    [[nodiscard]] ws::app::UndoStack& undo() override;
    // Also the document's serial: unique for the life of the process.
    [[nodiscard]] std::uint64_t job_group() const override;

    // Process-unique identity of THIS document object (never reused, never
    // 0). Schematic::version() cannot play this role - it is a per-instance
    // counter, so two documents routinely share a version. Anything that
    // outlives a document switch (the simulate job) keys on this.
    [[nodiscard]] std::uint64_t serial() const { return serial_; }

    [[nodiscard]] std::string autosave_content() const override;
    void adopt_autosave_content(std::string_view bytes) override;

    // The document. The editor's commands mutate it through the
    // model's documented writer paths (the *_mut() containers).
    ws::circsim::Schematic schem;

    // The last load's diagnostics (tolerant repairs, migration notes)
    // - the Problems panel shows them beside the ERC findings.
    ws::circsim::LoadReport report;

  private:
    std::string path_;
    ws::settings::FileSystem* fs_;
    ws::app::UndoStack undo_{200, 8u << 20};
    std::uint64_t serial_;
};

// --- the command builders (document.cpp; each pushes onto `doc.undo())
//
// Every one captures the inverse at construction and lands the edit
// through push() - callers never mutate the schematic directly.

// Places `part` as given (ref, params, rot, mirror already chosen).
void push_place_part(SchematicDocument& doc, ws::circsim::Part part);

// Removes the part plus every wire referencing its pins plus the
// junctions those wires orphan.
void push_delete_part(SchematicDocument& doc, std::uint32_t part_id);

// One attached wire's re-route (the move commands' extra cargo): the
// wire's id and the waypoints that keep its legs axis-aligned at the
// landing position. The editor computes the new shape (it owns the
// symbol geometry the endpoints resolve through); the command captures
// each wire's CURRENT waypoints as the undo shape. The lib's own type
// now (ws::circsim::WireRoute, geom.hpp - attached_wire_routes answers
// it); the alias keeps the editor's historical spelling.
using WireRoute = ws::circsim::WireRoute;

// A junction riding a moved part's pin (glued_junctions): the command
// carries its from/to POSITIONS - the pusher resolves the pin landings
// with the library in hand (the document owns no symbol knowledge).
// Without riding, the move strands the wire end's junction at the old
// spot and the connection the author drew dies.
struct GluedJunctionMove {
    std::uint32_t junction = 0;
    std::uint32_t pin = 0;  // the pin it rides (the pusher's landing
                             // resolution; the command never reads it)
    ws::circsim::Pos from{0.0, 0.0};
    ws::circsim::Pos to{0.0, 0.0};
};

// push_move_part: `to` is the part's landing; the command also carries
// the glued junctions' landings (glued_junctions - the junctions
// sitting exactly on the part's pins, whose from/to the CALLER has
// resolved with the library in hand: a moved pin lands at to + the
// pin's own offset, a rotated one wherever its transform puts it).
// Passing them keeps the wire ends' and T-taps' connections alive.
void push_move_part(SchematicDocument& doc, std::uint32_t part_id,
                    ws::circsim::Pos to,
                    std::vector<WireRoute> wire_routes = {},
                    std::vector<GluedJunctionMove> glued = {});

// rot/mirror are the FINAL values ("Rotate" or "Mirror" is the step's
// name - the undo label reads what the user did).
void push_transform_part(SchematicDocument& doc, std::uint32_t part_id,
                         int rot, bool mirror, std::string name);

// The explicit junctions a move/rotate's landing CREATED: for every
// contact the moved part's pins made with another wire's interior
// (ws::circsim::moved_pin_contacts), one junction is minted at the
// contact position - the pickup rule's silent connection becomes a
// named node with a drawn dot, so it survives the NEXT move away and
// the undo of the move that made it. One undo step, one name; an
// empty list lands nothing. Returns the number minted.
std::size_t push_contact_junctions(
    SchematicDocument& doc,
    const std::vector<ws::circsim::MovedPinContact>& contacts,
    std::string name);

// The routed seam (the rotate/mirror the editor lands after a drag):
// the glued junctions ride the step with both orientations' landings -
// each entry's `to` is the pin's spot at the NEW orientation (apply),
// its `from` the OLD one (undo). A rotated pin lands wherever its
// transform puts it; the pusher resolves both with the library in hand.
void push_transform_part_routed(SchematicDocument& doc, std::uint32_t part_id,
                                int rot, bool mirror, std::string name,
                                std::vector<WireRoute> wire_routes,
                                std::vector<GluedJunctionMove> glue = {});

// One part text field. kind: 0 = ref, 1 = value, 2 = params[param].
// Typing runs coalesce (merge_with) so one undo reverts the run.
void push_part_text(SchematicDocument& doc, std::uint32_t part_id, int kind,
                    std::string param, std::string next, std::string name);

// One source's DC/AC mode flip: the value AND the wave param land as
// ONE named step (the Properties panel's mode dropdown). The caller
// hands the FINAL pair - next_value and next_wave (an empty wave means
// none) - and the command captures both halves' current state, so a
// single undo reverts the whole switch instead of leaving a half-set
// hybrid (a wave beside a stale DC value) on the model.
void push_source_mode(SchematicDocument& doc, std::uint32_t part_id,
                      std::string next_value, std::string next_wave);

// The wire carries its endpoints and route; the junction (when the
// route ends on free space) is placed in the same named transaction.
void push_place_wire(SchematicDocument& doc, ws::circsim::Wire wire,
                     std::vector<ws::circsim::Junction> junctions);

// Removes the wire plus the junctions it orphans.
void push_delete_wire(SchematicDocument& doc, std::uint32_t wire_id);

// push_move_junction: the drag-release move of one junction. The
// binding changes the LANDING makes (tap_add: wires whose bodies the
// dot now sits on - the author dragged it there, the spec's "move the
// wire directly" case; tap_remove: taps whose wires no longer pass
// through the landing) ride the same undo step. The pusher computes
// them against the landed schematic (entity at `to`) with the library
// in hand; the command owns no symbol knowledge.
void push_move_junction(SchematicDocument& doc, std::uint32_t junction_id,
                        ws::circsim::Pos to,
                        std::vector<WireRoute> wire_routes = {},
                        std::vector<std::uint32_t> tap_add = {},
                        std::vector<std::uint32_t> tap_remove = {});

void push_place_label(SchematicDocument& doc, ws::circsim::Label label);
void push_delete_label(SchematicDocument& doc, std::uint32_t label_id);
void push_move_label(SchematicDocument& doc, std::uint32_t label_id,
                     ws::circsim::Pos to);
void push_label_text(SchematicDocument& doc, std::uint32_t label_id,
                     std::string next);

// The whole-text edits (typing coalesces like the part fields).
void push_directives(SchematicDocument& doc, std::string next);

// The Simulation tab's temperature row (§7.7): nullopt = SPICE's 27°C
// default, so the row clears back to it. Typing coalesces.
void push_set_temp(SchematicDocument& doc, std::optional<double> next);

// The analysis list: index-checked at push (a no-op refusal beyond
// the caller's own guards when the list changed under it).
void push_add_analysis(SchematicDocument& doc,
                       ws::circsim::AnalysisSpec spec);
void push_remove_analysis(SchematicDocument& doc, std::size_t index);
void push_set_analysis(SchematicDocument& doc, std::size_t index,
                       ws::circsim::AnalysisSpec next);
// The deck-order reorder (D3's up/down): one entry moves from `from`
// to `to`, one undo step for the move. Out-of-range or a same-slot
// move refuses as a no-op push - the family's own guard.
void push_move_analysis(SchematicDocument& doc, std::size_t from,
                        std::size_t to);

// The next free reference designator for a symbol ("R1", "R2", ...):
// the highest trailing number on the symbol's own prefix, plus one -
// no gap reuse, deterministic. Ref-less symbols (power/ground) answer
// "" (the symbol's contract). Lives in the lib now (ws::circsim::
// next_ref, geom.hpp); the using keeps the editor's historical
// unqualified spelling resolving.
using ws::circsim::next_ref;

// --- the group commands (the box-select's landing gear) ---------------------
//
// One member of a group move: the entity's kind and id with the
// from/to pair (the caller captures from at press, to at release -
// the single moves' own contract, widened to N entities).
struct EntityMove {
    enum class Kind : std::uint8_t { Part, Junction, Label };
    Kind kind = Kind::Part;
    std::uint32_t id = 0;
    ws::circsim::Pos from{0.0, 0.0};
    ws::circsim::Pos to{0.0, 0.0};
};

// The whole group's move as ONE undo step: every member lands its
// `to` (undo returns it to its `from`), and the attached wires'
// fresh-at-the-landing routes ride the same step beside their
// press-time undo shapes (a route whose wire is gone is skipped -
// the honest shape, the single move's own rule).
void push_move_set(SchematicDocument& doc, std::vector<EntityMove> moves,
                   std::vector<WireRoute> routes = {},
                   std::vector<GluedJunctionMove> glue = {});


// The paste's cargo: entities with PRE-MINTED ids (the editor mints
// them against the live schematic while building the payload, the
// same breath the canvas placements mint in), refs already
// renumbered, positions already offset - the command itself is the
// pure insert/remove pair.
struct PastePayload {
    std::vector<ws::circsim::Part> parts;
    std::vector<ws::circsim::Junction> junctions;
    std::vector<ws::circsim::Wire> wires;
    std::vector<ws::circsim::Label> labels;
};

// Inserts the payload as ONE undo step ("Paste"); undo removes
// exactly those ids, redo re-inserts them (the reinsert path: ids
// stay, next_id re-raises past them - the ids-are-never-reused
// rule's own loop hole).
void push_paste(SchematicDocument& doc, PastePayload payload);

// Removes a working set as ONE undo step: the named parts (each with
// its wires and the junctions those wires orphan - the part rule),
// the named wires (with their own orphaned junctions), the labels.
// Junction ids are not removed directly - a junction dies with its
// wires (the single-delete rule); a selected junction whose wire
// survives stays, because the wire still wants it. Returns false
// when the set named nothing the schematic has.
[[nodiscard]] bool push_delete_set(
    SchematicDocument& doc, const std::vector<std::uint32_t>& parts,
    const std::vector<std::uint32_t>& wires,
    const std::vector<std::uint32_t>& labels);

}  // namespace editor
