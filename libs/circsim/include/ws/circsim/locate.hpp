#pragma once

#include <optional>

#include <ws/circsim/diag.hpp>  // DiagTarget lives there (producers
                                // fill it; this seam only consumes it)
#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/nets.hpp>
#include <ws/circsim/symbols.hpp>

// Stage 3 (the editor): turning a Diag back into the object it points
// at, so the Problems panel can select and zoom.
//
// The PREFERRED path: producers (erc/nets/export) fill Diag::target -
// the structured jump target - at the source, and locate_diag returns
// it verbatim. The LEGACY path (kept so un-migrated producers keep
// working): the diag carries its target only as the pre-rendered
// `where` string ("R3", "R3 pin b", "npn 7", "net N004", "directives",
// "" = file-level) - the format the CLI and the Problems panel show -
// and locate_diag parses that spelling against the schematic plus the
// export back-map: element names resolve through
// SpiceExport::element_part (the mangled included), net names through
// NetList::by_name and the export's node_net, pins by name through the
// symbol table. Nothing here guesses: an unresolvable spelling answers
// Kind::None and the panel shows the row without a jump target.
//
// focus_bounds() is the zoom answer: the world-space AABB of the
// target - a part's transformed symbol bounds, a net's member extent -
// in grid units (y up, the model's frame).

namespace ws::circsim {

// Resolves a Diag to its jump target. A Diag whose `target` is set
// answers it directly (the producers' structured path). Otherwise this
// parses `d.where` against the schematic: `nets` (nullable) resolves
// net names; `map` (nullable) resolves element names through the export
// back-map - the editor passes both when it has them (an ERC pass has
// nets, a run outcome has nets + map); `symbols` (nullable, null = the
// built-in table) resolves pin names. The `where` spellings are the
// producers' own (erc/nets/export): a ref ("R3") or "symbol id"
// ("npn 7") or "part 12"; the pin tail " pin b"; "net N004";
// "wire 5"; "label out"; "directives"; "analysis setup"; a comma
// list ("V1, L1" - the loop findings) answers the FIRST token that
// resolves. An unresolvable spelling answers Kind::None.
[[nodiscard]] DiagTarget locate_diag(const Diag& d, const Schematic& s,
                                    const NetList* nets,
                                    const struct SpiceExport* map,
                                    const SymbolLibrary* symbols = nullptr);

// The zoom target: the world-space bounds of `t` (grid units, y up).
// A part answers its transformed symbol bounds (symbol_bounds covers
// pins and prims); a pin adds nothing (the part already contains it -
// the editor highlights the pin itself). A net answers the extent of
// its member pins, wire endpoints, junctions and labels; a wire its
// polyline; a label its anchor (grown one unit so a fit is never
// degenerate). Directives, Analysis, None answer nullopt (nothing to
// zoom to). `supplied_nets` (nullable) saves a net build for the Net
// case when the caller already has one (the editor's ERC cache); null
// builds one here - a click-path cost, never a frame path.
[[nodiscard]] std::optional<Bounds> focus_bounds(
    const DiagTarget& t, const Schematic& s,
    const NetList* supplied_nets = nullptr,
    const SymbolLibrary* symbols = nullptr);

}  // namespace ws::circsim
