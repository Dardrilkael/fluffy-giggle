#pragma once

#include <string>
#include <vector>

// The Results API (DESIGN.md §7.8): per analysis a Table - analysis kind,
// sweep column kind, column names as spelled, row-major samples. Real
// values today; the complex twin for .ac rides the same shape in M2.
// Nothing here references ws::math (the backend rule, §2.2); writers in
// src/output.cpp consume Table only, so a future .raw writer is just
// another consumer of the same type.

namespace ws::spice {

enum class AnalysisKind {
    Op,   // §7.2
    Dc,   // §7.3
};

// What the first column(s) of the table measure.
enum class SweepKind {
    None,       // .op: no sweep column
    Swept,      // .dc: one column per swept source (inner first)
    Time,       // .tran (M2)
    Frequency,  // .ac (M2)
};

struct Table {
    AnalysisKind analysis = AnalysisKind::Op;
    SweepKind sweep = SweepKind::None;

    // Sweep column headers, as spelled in the deck (e.g. "V1", "temp");
    // empty for SweepKind::None. Nested .dc sweeps get one column each,
    // innermost first (ngspice order, §7.3).
    std::vector<std::string> sweep_names;

    // Saved quantities after the sweep column(s), named as the deck
    // spells them: v(out), i(V1) - the default save-list is all node
    // voltages plus all voltage-source branch currents (§0.5).
    std::vector<std::string> columns;

    // Row-major: each row is sweep value(s) then one value per column.
    std::vector<std::vector<double>> rows;
};

struct Results {
    std::vector<Table> tables;
};

// CSV per §0.5: header row, then one row per sample, comma separated,
// sweep column(s) first. Numbers print as %.12g (round-trip stable for
// double comparisons and deterministic across runs, §10.5).
[[nodiscard]] std::string table_csv(const Table& t);

// The whole run as one CSV body: tables in deck order, joined by a blank
// line. Per §7.2 an .op table is written only when it is the run's sole
// analysis (one row, header + values) - inside a multi-analysis run its
// numbers still live in Results, they just do not get a CSV section.
[[nodiscard]] std::string results_csv(const Results& r);

}  // namespace ws::spice
