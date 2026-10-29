// Results writers (DESIGN.md §2.2, §7.8): consume Table only. The CSV
// shape is §0.5 - header row, sweep column(s) first, one column per saved
// quantity named as the deck spells it, comma separated. Numbers print
// %.12g: round-trip stable for the test suite's comparisons and
// deterministic run to run (§10.5). Columnar storage means the writer
// walks columns by sample index - and so will the .raw writer (a future
// consumer of the same type, no reshape needed).

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

}  // namespace

std::string table_csv(const Table& t) {
    std::string out;
    // §0.5: sweep column(s) first, then one column per saved quantity -
    // a plain comma-join. A .op table (SweepKind::None, no sweep_names)
    // starts directly with its signal columns; the header must always
    // have exactly as many fields as every data row.
    bool first = true;
    for (const std::string& name : t.sweep_names) {
        if (!first) {
            out += ',';
        }
        out += name;
        first = false;
    }
    for (const Signal& s : t.signals) {
        if (!first) {
            out += ',';
        }
        out += s.name;
        first = false;
    }
    out += '\n';

    const std::size_t n = t.row_count();
    for (std::size_t i = 0; i < n; ++i) {
        bool row_first = true;
        for (const auto& ax : t.sweep_axes) {
            if (!row_first) {
                out += ',';
            }
            append_number(out, i < ax.size() ? ax[i] : 0.0);
            row_first = false;
        }
        for (const Signal& s : t.signals) {
            if (!row_first) {
                out += ',';
            }
            append_number(out, i < s.data.size() ? s.data[i] : 0.0);
            row_first = false;
        }
        out += '\n';
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
