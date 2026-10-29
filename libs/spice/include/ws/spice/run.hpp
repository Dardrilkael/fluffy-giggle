#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/results.hpp>

// One run of a deck (DESIGN.md §2.1, §7): analyses execute in deck order,
// each on a fresh elaboration of the AST (deck order never changes an
// answer, §7.1), each producing its own Table plus the §9.4 diagnostics
// record. This is the whole public entry for M1; the Simulator value that
// owns multi-analysis state (op reuse, .step families) grows here without
// changing the shape.

namespace ws::spice {

struct DeckAst;

// Tolerances and iteration caps, .options-tunable (§5.1). Defaults follow
// SPICE: reltol 1e-3, vntol 1e-6 V, abstol 1e-12 A, gmin 1e-12, itl1 100
// (op), itl2 50 (dc sweep point), itl4 10 (tran time point), chgtol 1e-14
// C, trtol 7 (the transient LTE tolerance, §6.3).
struct RunSettings {
    double reltol = 1e-3;
    double abstol = 1e-12;
    double vntol = 1e-6;
    double gmin = 1e-12;
    int itl1 = 100;  // .op
    int itl2 = 50;   // .dc sweep point
    int itl4 = 10;   // .tran time point
    double chgtol = 1e-14;  // §6.3: charge/flux LTE floor
    double trtol = 7.0;     // §6.3: transient LTE tolerance factor
    std::string solver = "eigen";  // M0b flips the default to "klu"

    // §7.7: run temperature / model nominal temperature, °C. The circuit
    // elaborates at `temp` (a `.dc temp` / `.step temp` point re-elaborates
    // at ITS value); M1/M2 devices are temp-invariant, temp-dependent
    // models land M5.
    double temp = 27.0;
    double tnom = 27.0;

    // Test-only fixed-step hook (§10.1 trap-order row): when > 0, .tran
    // marches at exactly this dt - pure trapezoidal (no BE restarts) and
    // no LTE rejection - so halving studies are meaningful. Breakpoints
    // still truncate a step so waveform edges are never stepped over.
    double fixed_dt = 0.0;
};

// The per-analysis diagnostics record (§9.4), minimal for M1: the ladder
// fields (gmin/source stepping, worst unknowns) land with M3. M2 adds the
// transient step counters.
struct AnalysisReport {
    std::string analysis;              // ".op", ".dc", ".tran", ".ac"
    std::size_t newton_iterations = 0; // total across the analysis
    std::size_t lu_factorizations = 0; // refactors over the run
    std::string stage = "newton";      // ladder stage reached (M3 grows it)
    std::size_t accepted_steps = 0;    // .tran: accepted time points
    std::size_t rejected_steps = 0;    // .tran: LTE-rejected attempts
};

struct RunOutput {
    // One Table per analysis card, in deck order.
    Results results;
    std::vector<AnalysisReport> reports;  // parallel to results.tables
};

// Runs every analysis in the deck. `settings` carries the caller's
// defaults; .options cards in the deck override them (later cards win,
// §5.1). Warnings (ignored .options keys, ...) append to `warnings`,
// never fatal (§9.3). Deck-level failures (parse already done by the
// caller; elaboration, topology, singular, convergence) return as
// ws::core::Error carrying a rendered Diagnostic (§9.2).
[[nodiscard]] ws::core::Result<RunOutput> run_deck(const DeckAst& ast,
                                                   RunSettings settings,
                                                   std::vector<Warning>& warnings);

}  // namespace ws::spice
