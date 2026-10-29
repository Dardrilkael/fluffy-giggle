#pragma once

// Internal (src-only): the per-analysis drivers behind BOTH front ends -
// run_deck (netlist) and Simulator (builder/handle). Each takes an
// immutable Circuit plus the run's RunState; `ast` (nullable) is the
// elaboration source a temperature sweep needs to re-elaborate from -
// a Simulator-created circuit passes null and temp sweeps report a Usage
// error naming the limitation instead of guessing.

#include <string>
#include <vector>

#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice::drivers {

// §7.2
[[nodiscard]] ws::core::Result<Table> run_op(const Circuit& circuit, RunState& state,
                                             const RunSettings& settings,
                                             AnalysisReport& report);

// §7.3. `ast` may be null (no temp re-elaboration possible).
[[nodiscard]] ws::core::Result<Table> run_dc(const DeckAst* ast,
                                             const Circuit& base_circuit,
                                             const DcCard& card,
                                             const RunSettings& settings,
                                             std::vector<Warning>& warnings,
                                             AnalysisReport& report);

// §6
[[nodiscard]] ws::core::Result<Table> run_tran(const Circuit& circuit, RunState& state,
                                               const TranCard& card,
                                               const RunSettings& settings,
                                               const ParamTable& scope,
                                               const std::vector<IcCard>& ics,
                                               AnalysisReport& report);

// §7.4
[[nodiscard]] ws::core::Result<Table> run_ac(const Circuit& circuit, RunState& state,
                                             const AcCard& card,
                                             const RunSettings& settings,
                                             const ParamTable& scope,
                                             AnalysisReport& report);

}  // namespace ws::spice::drivers
