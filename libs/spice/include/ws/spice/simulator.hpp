#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/analysis.hpp>
#include <ws/spice/builder.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>
#include <ws/spice/transient.hpp>

// The Simulator (DESIGN.md §2.1 rev): one value owning a run over an
// immutable Circuit. The CLI's run_deck stays for whole-deck runs; this
// is the C++ surface - the primary, tested one:
//
//   auto sim = ws::spice::Simulator::create(std::move(circuit)).value();
//   ws::spice::Table res = sim.run(Tran{.tstop = 5_ms}).value();
//   sim.set(R1.value, 22_k);            // handle-based value change
//   auto again = sim.run(Tran{.tstop = 5_ms}).value();
//
// set()/set_source() write bound parameter slots - values only, so the
// symbolic pattern (and the LU analyze behind it) is reused by
// construction: a value change can never move a stamp position (§3.5
// rev). The run's dynamic state lives in the Simulator's RunState;
// snapshot()/restore() copy the whole arena (rewind, retry, checkpoint).
//
// A steppable transient (advance_to / cancellation / progress) is the
// TransientRun returned by begin_transient().

namespace ws::spice {

class TransientRun;

class Simulator {
public:
    Simulator(Simulator&&) = default;
    Simulator& operator=(Simulator&&) = default;

    // Binds the circuit's parameter expressions against its own scope and
    // seeds the state arena. Fails on a circuit whose parameters do not
    // evaluate (the same check a deck run would make).
    [[nodiscard]] static ws::core::Result<Simulator> create(Circuit circuit,
                                                            RunSettings settings = {});

    // Runs one analysis on the CURRENT bound values. The transient
    // re-seeds its t=0 state each call (§3.5 rev's trivial reset);
    // parameters keep whatever set() last wrote. Returns the analysis's
    // table; last_report() carries its §9.4 diagnostics record.
    [[nodiscard]] ws::core::Result<Table> run(const Analysis& a);

    // Handle-based parameter write: pins the slot (survives nothing else
    // - the Simulator owns one state) and updates the bound value. A
    // Usage error names the handle when the device or parameter does not
    // exist.
    [[nodiscard]] ws::core::Status set(const ParamHandle& h, double value);

    // Pins a source's DC value OVER its waveform (the closed-loop hook:
    // a controller writes set_source() between transient steps). The
    // source's `force` slot is set, so PULSE/SIN/... no longer drive it
    // until set_source(name, waveform) releases the pin.
    [[nodiscard]] ws::core::Status set_source(std::string_view source, double value);
    // Releases the force pin; the construction waveform drives again.
    [[nodiscard]] ws::core::Status release_source(std::string_view source);

    // A steppable, cancellable transient over THIS Simulator's state
    // (transient.hpp): advance_to(t) marches whole accepted steps;
    // set()/set_source() between calls drive the next step - the
    // closed-loop pattern. The Simulator must outlive the run and must
    // not run other analyses while it is live (they share the state).
    [[nodiscard]] ws::core::Result<TransientRun> begin_transient(
        const TranAnalysis& a, const std::vector<IcNode>& ics = {});

    // The whole run state as a value: parameters + dynamic state + pins.
    // Copying IS snapshot/restore (§3.5 rev).
    [[nodiscard]] RunState snapshot() const { return state_; }
    void restore(RunState st) { state_ = std::move(st); }

    // Rebinds every parameter expression against `scope` (a .step point,
    // a Monte-Carlo draw - pass the Rand so gauss()/flat() draw), then
    // re-applies pins. The symbolic analysis is NOT redone (§7.6 rev).
    [[nodiscard]] ws::core::Status rebind(const ParamTable* scope = nullptr,
                                          Rand* rng = nullptr);

    [[nodiscard]] const Circuit& circuit() const { return *circuit_; }
    [[nodiscard]] const RunSettings& settings() const { return settings_; }
    [[nodiscard]] const AnalysisReport& last_report() const { return last_report_; }
    [[nodiscard]] const std::vector<Warning>& warnings() const { return warnings_; }

    // The bound value of one named parameter (diagnostics, assertions).
    [[nodiscard]] std::optional<double> param_value(const ParamHandle& h) const;

private:
    Simulator() = default;
    std::unique_ptr<Circuit> circuit_;
    RunSettings settings_;
    RunState state_;
    AnalysisReport last_report_;
    std::vector<Warning> warnings_;
};

}  // namespace ws::spice
