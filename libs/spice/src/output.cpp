// Results writers (DESIGN.md §2.2, §7.8): consume Table only. The CSV
// shape is §0.5 - header row, sweep column(s) first, one column per saved
// quantity named as the deck spells it, comma separated. Numbers print
// %.12g: round-trip stable for the test suite's comparisons and
// deterministic run to run (§10.5).

#include <cstdio>
#include <string>
#include <vector>

#include <ws/spice/results.hpp>

namespace ws::spice {

namespace {

void append_number(std::string& out, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    out += buf;
}

void append_row(std::string& out, const std::vector<double>& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        append_number(out, row[i]);
    }
    out += '\n';
}

}  // namespace

std::string table_csv(const Table& t) {
    std::string out;
    for (std::size_t i = 0; i < t.sweep_names.size(); ++i) {
        out += ',';
        out += t.sweep_names[i];
    }
    for (const std::string& col : t.columns) {
        out += ',';
        out += col;
    }
    out += '\n';
    for (const std::vector<double>& row : t.rows) {
        append_row(out, row);
    }
    return out;
}

std::string results_csv(const Results& r) {
    std::string out;
    bool wrote_any = false;
    for (const Table& t : r.tables) {
        // §7.2: .op writes CSV only when it is the run's sole analysis.
        if (t.analysis == AnalysisKind::Op && r.tables.size() > 1) {
            continue;
        }
        if (wrote_any) {
            out += '\n';  // blank line between sections
        }
        out += table_csv(t);
        wrote_any = true;
    }
    return out;
}

}  // namespace ws::spice
