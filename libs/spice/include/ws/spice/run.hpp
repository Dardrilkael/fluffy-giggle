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
// (op), itl2 50 (dc sweep point).
struct RunSettings {
    double reltol = 1e-3;
    double abstol = 1e-12;
    double vntol = 1e-6;
    double gmin = 1e-12;
    int itl1 = 100;  // .op
    int itl2 = 50;   // .dc sweep point
    std::string solver = "eigen";  // M0b flips the default to "klu"
};

// The per-analysis diagnostics record (§9.4), minimal for M1: the ladder
// fields (gmin/source stepping, worst unknowns) land with M3.
struct AnalysisReport {
    std::string analysis;              // ".op", ".dc"
    std::size_t newton_iterations = 0; // total across the analysis
    std::size_t lu_factorizations = 0; // refactors over the run
    std::string stage = "newton";      // ladder stage reached (M3 grows it)
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
