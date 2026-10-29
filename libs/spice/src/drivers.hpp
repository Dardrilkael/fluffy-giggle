#pragma once

// Internal (src-only): the per-analysis drivers behind BOTH front ends -
// run_deck (netlist) and Simulator (builder/handle). Each takes an
// immutable Circuit plus the run's RunState; `ast` (nullable) is the
// elaboration source a temperature sweep needs to re-elaborate from -
// a Simulator-created circuit passes null and temp sweeps report a Usage
// error naming the limitation instead of guessing.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>
#include <ws/spice/transient.hpp>

namespace ws::spice::drivers {

// The time loop behind run_tran AND TransientRun (§6 rev): prepare()
// validates the card and seeds t=0; step() marches exactly one accepted
// step; hooks cancel or observe per attempt/accept.
class TranMarch {
public:
    struct Hooks {
        const StopToken* stop = nullptr;                // cancel (prompt)
        std::function<bool(double now, double tstop)> on_accept;  // false = stop
    };

    TranMarch(const Circuit& circuit, RunState& state, const RunSettings& settings,
              const TranCard& card, const ParamTable& scope,
              const std::vector<IcCard>& ics);
    ~TranMarch();
    TranMarch(TranMarch&&) = default;

    ws::core::Status prepare();
    ws::core::Status step(const Hooks& hooks);

    [[nodiscard]] bool done() const;
    [[nodiscard]] bool stopped() const;  // a hook asked to stop (resumable)
    [[nodiscard]] double now() const;
    [[nodiscard]] double tstop() const;
    [[nodiscard]] Table table() const;
    [[nodiscard]] Table take_table();
    [[nodiscard]] AnalysisReport report() const;
    [[nodiscard]] const std::vector<double>& solution() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
