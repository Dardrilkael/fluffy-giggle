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
    [[nodiscard]] std::uint64_t job_group() const override;

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
// each wire's CURRENT waypoints as the undo shape.
struct WireRoute {
    std::uint32_t wire = 0;
    std::vector<ws::circsim::Pos> waypoints;
};

void push_move_part(SchematicDocument& doc, std::uint32_t part_id,
                    ws::circsim::Pos to,
                    std::vector<WireRoute> wire_routes = {});

// rot/mirror are the FINAL values ("Rotate" or "Mirror" is the step's
// name - the undo label reads what the user did).
void push_transform_part(SchematicDocument& doc, std::uint32_t part_id,
                         int rot, bool mirror, std::string name);

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

void push_move_junction(SchematicDocument& doc, std::uint32_t junction_id,
                        ws::circsim::Pos to,
                        std::vector<WireRoute> wire_routes = {});

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

// The next free reference designator for a symbol ("R1", "R2", ...):
// the highest trailing number on the symbol's own prefix, plus one -
// no gap reuse, deterministic. Ref-less symbols (power/ground) answer
// "" (the symbol's contract).
[[nodiscard]] std::string next_ref(const ws::circsim::Schematic& s,
                                   const ws::circsim::SymbolDef& sym);

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
                   std::vector<WireRoute> routes = {});

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
