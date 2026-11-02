#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/circsim/diag.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/nets.hpp>
#include <ws/circsim/symbols.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>

// The ws::spice export (apps/circsim stage 1). The schematic becomes a
// netlist deck - title, the schematic's verbatim directives block (its
// .model/.subckt/.include text), one card per exported part (sorted by
// ref; power/ground parts emit nothing - their fixed net names already
// carry them), the analysis cards, .end - and that text goes through
// ws::spice's own front end (read_deck -> build_deck_ast -> run_deck):
// the ONE simulation engine, no second solver anywhere.
//
// The back-map ties the deck to the drawing: element name -> part id,
// sanitized node token -> net id, and deck line -> origin, so later stages
// turn spice diagnostics into Problems-panel entries that select and zoom
// to the object.

namespace ws::circsim {

// Case-insensitive less-than for the map keys.
struct CiLess {
    [[nodiscard]] bool operator()(const std::string& a,
                                  const std::string& b) const;
};

struct SpiceExport {
    std::string text;
    std::string deck_name = "circsim export";  // diagnostics' file spelling

    std::map<std::string, std::uint32_t, CiLess> element_part;  // "R1" -> id
    std::map<std::string, std::uint32_t, CiLess> node_net;      // token -> id

    struct Origin {
        enum class Kind { None, Part, Directives, Analysis };
        Kind kind = Kind::None;
        std::uint32_t id = 0;  // part id when kind == Part
    };
    // Deck line n (1-based) -> where it came from; index 0 is line 1.
    std::vector<Origin> line_origins;
};

// Builds the deck. `analyses` overrides the schematic's stored setup
// (null = use it; an empty vector emits no analysis cards - the caller
// decides the default). Fails when two distinct nets sanitize to the same
// node token (the message names them); net-build problems ride in `diags`
// and never fail the export.
[[nodiscard]] ws::core::Result<SpiceExport> export_deck(
    const Schematic& s, const NetList& nets,
    const std::vector<AnalysisSpec>* analyses = nullptr,
    std::vector<Diag>* diags = nullptr);

// The analysis card text for one spec: ".op", ".dc V1 0 5 0.1",
// ".tran 1u 1m", ".ac dec 10 1 1meg" (numbers print %.12g; a TranSpec
// with tstep 0 spells tstop/100, matching ws::spice's card semantics).
[[nodiscard]] std::string analysis_card(const AnalysisSpec& a);

// Parses one CLI --analysis argument: "op" | "dc V1 0 5 0.1" |
// "tran 1u 1m [tstart [tmax]] [uic]" | "ac dec 10 1 100k" - values use
// the ws::spice number grammar (1u, 100k, 1meg).
[[nodiscard]] ws::core::Result<AnalysisSpec> parse_analysis_spec(
    std::string_view text);

// The full path: build nets -> export -> read_deck -> build_deck_ast ->
// run_deck. Never throws; the outcome carries both halves of a failure -
// the raw ws::core::Error in `run`, and the same problem mapped onto
// schematic objects in `problems` (deck line -> part, element/node names
// matched against the back-map).
struct RunOutcome {
    ws::core::Result<ws::spice::RunOutput> run;  // failed: the raw error
    SpiceExport map;
    NetList nets;
    std::vector<Diag> problems;  // net diags + the mapped failure
    std::vector<ws::spice::Warning> warnings;  // spice's own, unmapped
};

[[nodiscard]] RunOutcome run_schematic(
    const Schematic& s,
    const std::vector<AnalysisSpec>* override_analyses = nullptr,
    const ws::spice::IncludeResolver& include = {});

// The CLI's --format json writer: {"format":"circsim-results",...} with
// one object per table (analysis kind, sweep names, columns, rows).
[[nodiscard]] std::string results_json(const ws::spice::RunOutput& run);

}  // namespace ws::circsim
