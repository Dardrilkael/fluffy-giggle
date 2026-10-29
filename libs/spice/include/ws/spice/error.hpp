#pragma once

#include <string>
#include <vector>

#include <ws/core/result.hpp>

// ws::spice error taxonomy (DESIGN.md §9). Every failure a deck or a run
// can produce is one of these kinds, rendered in the §9.2 message shape and
// carried into ws::core::Error at the public boundary - no exceptions cross
// the lib edge (house rule), and library code never exits/aborts (§0.6).

namespace ws::spice {

enum class ErrorKind {
    Parse = 1,       // lexing, grammar, numbers, unknown cards
    Semantic = 2,    // unknown model/node/param, bad arity, include cycle
    Topology = 3,    // §6.6 graph findings: floating nodes, V loops, ...
    Convergence = 4, // Newton failure, NaN iterate
    Singular = 5,    // factorization failure
    Timestep = 6,    // dt below dtmin
    Measure = 7,     // .meas evaluation
    Io = 8,          // file I/O
    Usage = 9,       // CLI/API misuse
};

// A source coordinate through .include expansion (§8.1). `file` is the path
// as spelled on the .include card (or the deck's own path); line/col are
// 1-based, col in bytes.
struct SourceLoc {
    std::string file;
    int line = 0;
    int col = 0;
    bool valid = false;

    static SourceLoc make(std::string file, int line, int col) {
        return SourceLoc{std::move(file), line, col, true};
    }
};

// The structured payload that travels with an error (§9.1) - what a caller
// rendering its own diagnostics reads, instead of parsing the message text.
struct Diagnostic {
    ErrorKind kind = ErrorKind::Parse;
    std::string code;      // stable machine code, e.g. "spice.parse.unknown_card"
    std::string message;   // the human sentence, no prefix
    std::string hint;      // optional actionable follow-up
    SourceLoc loc;         // optional; when absent the error is deck-global
    std::string analysis;  // optional context, e.g. ".dc V1 at 3.30 V"
    std::vector<std::string> names;  // offending devices/nodes, if any

    // The §9.2 rendering:
    //   file:line:col: error: message      (with a source location)
    //   error: [analysis] message          (without)
    // each followed by "  hint: ..." when a hint is known.
    [[nodiscard]] std::string render() const;
};

// Wraps a Diagnostic into the ws::core::Error that Result APIs propagate:
// code = int(ErrorKind), category = "spice", message = the rendering. The
// structured half stays available through the Simulator's error accessor.
[[nodiscard]] ws::core::Error make_error(Diagnostic d);

// Convenience: an unexpected<Error> carrying d, for `return spice_fail(...)`.
[[nodiscard]] ws::core::unexpected<ws::core::Error> spice_fail(Diagnostic d);

// Collected, never fatal (§9.3); printed to stderr and the report,
// promoted to errors by the CLI's --werror.
struct Warning {
    Diagnostic diag;
};

}  // namespace ws::spice
