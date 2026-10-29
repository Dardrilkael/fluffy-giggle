#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

// The Results API (DESIGN.md §7.8, revised 2026-10-29): per analysis a
// Table - analysis kind, one shared sweep axis per swept quantity, and
// ONE CONTIGUOUS COLUMN PER SIGNAL. Columnar, not row-major: what
// plotting, interpolation, .meas and the .raw writer actually consume is
// whole signals (res["v(out)"] -> span<const double>), and a column is
// memcpy-able, cache-friendly, and exactly the .raw payload layout.
//
// Storage invariants:
//   * sweep axes: zero (.op), one (.tran/.ac/.plain .dc) or two (nested
//     .dc) columns, each with row_count() samples in row order.
//   * every signal column holds exactly row_count() samples.
//   * row_count() == 0 only for a signal-less table (never in practice).
//
// A .ac table stays real-valued in its public shape (the locked CSV
// decision): its signals are named re(x)/im(x). Nothing here references
// ws::math (the backend rule, §2.2); writers in src/output.cpp consume
// Table only, so the CSV writer and the .raw writer are just two
// consumers of the same type.

namespace ws::spice {

enum class AnalysisKind {
    Op,    // §7.2
    Dc,    // §7.3
    Tran,  // §6
    Ac,    // §7.4
};

// What the sweep axis/axes of the table measure.
enum class SweepKind {
    None,       // .op: no sweep axis
    Swept,      // .dc: one axis per swept source (inner first)
    Time,       // .tran
    Frequency,  // .ac
};

// One named signal: a contiguous column of row_count() samples.
struct Signal {
    std::string name;       // as spelled: v(out), i(V1), re(v(out)) ...
    std::vector<double> data;
};

struct Table {
    AnalysisKind analysis = AnalysisKind::Op;
    SweepKind sweep = SweepKind::None;

    // Sweep axis headers ("time", "frequency", "V1"), innermost first
    // (ngspice order, §7.3); empty for SweepKind::None.
    std::vector<std::string> sweep_names;

    // The sweep axes themselves, parallel to sweep_names: axis k holds
    // row_count() values in row order. For a nested .dc both columns
    // repeat their values in the flattened grid's order.
    std::vector<std::vector<double>> sweep_axes;

    // The signals, in save-list order.
    std::vector<Signal> signals;

    // --- accessors -----------------------------------------------------------

    // The number of samples in every column.
    [[nodiscard]] std::size_t row_count() const {
        if (!sweep_axes.empty()) {
            return sweep_axes[0].size();
        }
        return signals.empty() ? 0 : signals[0].data.size();
    }

    // The samples of one sweep axis (k < sweep_names.size()); an empty
    // span when the table has no such axis.
    [[nodiscard]] std::span<const double> axis(std::size_t k = 0) const {
        if (k >= sweep_axes.size()) {
            return {};
        }
        return sweep_axes[k];
    }

    // The contiguous column of a signal by name (case-sensitive, as
    // spelled); an empty span when absent. This is the accessor
    // plotting/.meas/.raw consume: no copy, no stride.
    [[nodiscard]] std::span<const double> signal(std::string_view name) const {
        const Signal* s = find(name);
        if (s == nullptr) {
            return {};
        }
        return s->data;
    }
    [[nodiscard]] std::span<const double> operator[](std::string_view name) const {
        return signal(name);
    }

    // Index of a signal by name, or nullopt.
    [[nodiscard]] std::optional<std::size_t> signal_index(std::string_view name) const {
        for (std::size_t i = 0; i < signals.size(); ++i) {
            if (signals[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Convenience: one materialized row (sweep values first, then one
    // value per signal, in order). Row-major consumers (the CSV writer,
    // tests, interop glue) use this; hot paths read whole columns.
    [[nodiscard]] std::vector<double> row(std::size_t i) const {
        std::vector<double> out;
        out.reserve(sweep_names.size() + signals.size());
        for (const auto& ax : sweep_axes) {
            out.push_back(i < ax.size() ? ax[i] : 0.0);
        }
        for (const Signal& s : signals) {
            out.push_back(i < s.data.size() ? s.data[i] : 0.0);
        }
        return out;
    }

    // Convenience: the whole table materialized row-major (the shape the
    // v1 API exposed). Allocation per call - tests and writers only.
    [[nodiscard]] std::vector<std::vector<double>> rows() const {
        std::vector<std::vector<double>> out(row_count());
        for (std::size_t i = 0; i < row_count(); ++i) {
            out[i] = row(i);
        }
        return out;
    }

private:
    [[nodiscard]] const Signal* find(std::string_view name) const {
        for (const Signal& s : signals) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }
};

struct Results {
    std::vector<Table> tables;

    // The first table carrying a signal of this name (analysis order);
    // .step families name their tables identically, so this is usually
    // what a caller wants. Empty span when nothing matches.
    [[nodiscard]] std::span<const double> signal(std::string_view name) const {
        for (const Table& t : tables) {
            const std::span<const double> s = t.signal(name);
            if (!s.empty()) {
                return s;
            }
        }
        return {};
    }
    [[nodiscard]] std::span<const double> operator[](std::string_view name) const {
        return signal(name);
    }
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
