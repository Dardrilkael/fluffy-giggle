// Analysis drivers (DESIGN.md §7) - M1: .op and .dc. Each analysis runs
// on a fresh elaboration (deck order never changes an answer, §7.1),
// walks the §5.1 Newton loop over the shared MNA pattern (pattern dry run
// once per circuit, refactor per iteration), and fills its Table with the
// §0.5 default save-list. The convergence-aid ladder (gmin/source
// homotopy) lands with M3; every M1 device is linear, so plain Newton
// converges in two iterations (solve + confirm) and the ladder paths stay
// unexercised rather than missing.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "circuit.hpp"
#include "mna.hpp"

#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>

namespace ws::spice {

namespace {

Diagnostic run_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

std::string fmt_value(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// --- the default save-list (§0.5) ----------------------------------------------

struct SaveList {
    std::vector<std::string> columns;
    std::vector<std::size_t> nodes;      // node ids (v(name) columns)
    std::vector<std::size_t> branches;   // branch ids (i(name) columns)
};

SaveList default_save_list(const Circuit& c) {
    SaveList s;
    for (std::size_t i = 0; i < c.node_count(); ++i) {
        s.columns.push_back("v(" + c.node_names[i] + ")");
        s.nodes.push_back(i);
    }
    for (std::size_t di : c.branch_devices) {
        s.columns.push_back("i(" + c.devices[di]->name() + ")");
        s.branches.push_back(c.devices[di]->branch());
    }
    return s;
}

std::vector<double> extract_row(const Circuit& c, const SaveList& s,
                                const std::vector<double>& x) {
    std::vector<double> row;
    row.reserve(s.nodes.size() + s.branches.size());
    for (std::size_t n : s.nodes) {
        row.push_back(x[n]);
    }
    for (std::size_t b : s.branches) {
        row.push_back(x[c.node_count() + b]);
    }
    return row;
}

// --- the DC/OP Newton kernel (§5.1) ----------------------------------------------

class DcKernel {
public:
    DcKernel(const Circuit& circuit, const RunSettings& settings)
        : circuit_(circuit), settings_(settings),
          mna_(circuit.node_count(), circuit.branch_count) {}

    // Pattern dry run + symbolic analyze (§3.2): once per circuit.
    ws::core::Status prepare() {
        return mna_.setup(circuit_.devices);
    }

    // One Newton solve to convergence. `x` is the warm start in, the
    // solution out. `context` rides into any Diagnostic (§9.2), e.g.
    // ".dc V1 at 3.3 V".
    ws::core::Status solve(std::vector<double>& x, int max_iterations,
                           const std::string& context, std::size_t& iterations) {
        const std::size_t nc = circuit_.node_count();
        // Cold start: an unsized (or stale-sized) x means all zeros.
        if (x.size() != mna_.dim()) {
            x.assign(mna_.dim(), 0.0);
        }
        for (int it = 1; it <= max_iterations; ++it) {
            ++iterations;
            mna_.load(circuit_.devices);
            std::vector<double> x_new;
            WS_TRY(mna_.solve(x_new));

            for (const double v : x_new) {
                if (!std::isfinite(v)) {
                    Diagnostic d = run_diag(
                        ErrorKind::Convergence, "spice.convergence.nan",
                        "Newton iterate is not finite");
                    d.analysis = context;
                    return spice_fail(std::move(d));
                }
            }

            // §5.1: |dx| <= reltol*max(|new|,|old|) + tol_abs per unknown,
            // tol_abs = vntol on voltages, abstol on branch currents.
            bool converged = true;
            for (std::size_t i = 0; i < x_new.size(); ++i) {
                const double tol = settings_.reltol *
                                       std::max(std::abs(x_new[i]), std::abs(x[i])) +
                                   (i < nc ? settings_.vntol : settings_.abstol);
                if (std::abs(x_new[i] - x[i]) > tol) {
                    converged = false;
                    break;
                }
            }
            x = std::move(x_new);
            if (converged) {
                return {};
            }
        }
        Diagnostic d = run_diag(
            ErrorKind::Convergence, "spice.convergence.newton",
            "no convergence after " + std::to_string(max_iterations) +
                " Newton iterations (gmin/source stepping lands with M3)");
        d.analysis = context;
        return spice_fail(std::move(d));
    }

    [[nodiscard]] std::size_t factorizations() const { return mna_.factorization_count(); }

private:
    const Circuit& circuit_;
    const RunSettings& settings_;
    MnaSystem mna_;
};

ws::core::unexpected<ws::core::Error> empty_circuit_error(const std::string& analysis) {
    Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.empty_circuit",
                            "no devices to simulate");
    d.analysis = analysis;
    d.hint = "a deck needs device cards (R, V, I, ...) before its analyses";
    return spice_fail(std::move(d));
}

// A solver failure surfacing from the ws::math facade (category "math":
// factorization, dimension, pattern) keeps its message but gains the spice
// taxonomy + analysis context (§9.1/§9.2). Errors the kernel already
// classified as a spice Diagnostic (category "spice": convergence, NaN
// iterate, ...) pass through UNTOUCHED - re-wrapping them re-branded
// Convergence as Singular, doubled the "error: [analysis]" prefix, and
// lost the original code (spice.convergence.nan/newton).
ws::core::unexpected<ws::core::Error> wrap_solver_error(const ws::core::Status& st,
                                                        const std::string& context) {
    if (std::string_view(st.error().category) == "spice") {
        return ws::core::unexpected(st.error());
    }
    Diagnostic d = run_diag(ErrorKind::Singular, "spice.singular",
                            st.error().message.empty() ? "matrix factorization failed"
                                                       : st.error().message);
    d.analysis = context;
    return spice_fail(std::move(d));
}

// --- .op (§7.2) ---------------------------------------------------------------------

ws::core::Result<Table> run_op(const DeckAst& ast, const RunSettings& settings,
                               std::vector<Warning>& warnings, AnalysisReport& report) {
    WS_TRY_ASSIGN(Circuit circuit, elaborate_deck(ast, kDefaultTemp, warnings));
    if (circuit.devices.empty()) {
        return empty_circuit_error(".op");
    }

    DcKernel kernel(circuit, settings);
    WS_TRY(kernel.prepare());

    std::vector<double> x;  // cold start: all zeros
    std::size_t iterations = 0;
    if (const ws::core::Status st = kernel.solve(x, settings.itl1, ".op", iterations);
        !st) {
        return wrap_solver_error(st, ".op");
    }

    report.analysis = ".op";
    report.newton_iterations = iterations;
    report.lu_factorizations = kernel.factorizations();
    report.stage = "newton";

    Table t;
    t.analysis = AnalysisKind::Op;
    t.sweep = SweepKind::None;
    t.columns = default_save_list(circuit).columns;
    t.rows.push_back(extract_row(circuit, default_save_list(circuit), x));
    return t;
}

// --- .dc (§7.3) -----------------------------------------------------------------------

struct SweepPlan {
    Token source;    // as spelled ("V1", "I2", "temp")
    bool is_temp = false;
    double start = 0.0;
    double stop = 0.0;
    double step = 0.0;
    std::size_t count = 0;  // floor((stop-start)/step + 1e-9) + 1
    char unit = 'V';        // 'V', 'A' or 'C' - for the error context
};

ws::core::Result<double> eval_sweep_expr(const ExprProgram& prog,
                                         const ParamTable& params) {
    WS_TRY_ASSIGN(const std::vector<std::optional<double>> bindings, params.bind(prog));
    WS_TRY_ASSIGN(const double v, prog.evaluate(bindings));
    return v;
}

ws::core::Result<SweepPlan> plan_sweep(const DcSweep& sweep, const ParamTable& params,
                                       const Circuit& circuit) {
    SweepPlan plan;
    plan.source = sweep.source;

    WS_TRY_ASSIGN(plan.start, eval_sweep_expr(sweep.start, params));
    WS_TRY_ASSIGN(plan.stop, eval_sweep_expr(sweep.stop, params));
    WS_TRY_ASSIGN(plan.step, eval_sweep_expr(sweep.step, params));

    if (plan.step == 0.0) {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.dc_step",
                                "sweep step for '" + sweep.source.text + "' is zero");
        d.loc = sweep.source.loc;
        return spice_fail(std::move(d));
    }
    if ((plan.stop > plan.start && plan.step < 0.0) ||
        (plan.stop < plan.start && plan.step > 0.0)) {
        Diagnostic d = run_diag(
            ErrorKind::Semantic, "spice.semantic.dc_direction",
            "sweep step for '" + sweep.source.text + "' must point toward " +
                fmt_value(plan.stop) + " (start " + fmt_value(plan.start) + ", step " +
                fmt_value(plan.step) + ")");
        d.loc = sweep.source.loc;
        return spice_fail(std::move(d));
    }
    plan.count = static_cast<std::size_t>(
                     std::floor((plan.stop - plan.start) / plan.step + 1e-9)) +
                 1;

    // Classify the target: temp re-elaborates per point (§7.7); anything
    // else must name a V or I source in the circuit.
    const std::string lower = to_lower(sweep.source.text);
    if (lower == "temp") {
        plan.is_temp = true;
        plan.unit = 'C';
        return plan;
    }
    for (const auto& dev : circuit.devices) {
        if (to_lower(dev->name()) != lower) {
            continue;
        }
        if (dev->kind() != 'V' && dev->kind() != 'I') {
            Diagnostic d = run_diag(
                ErrorKind::Semantic, "spice.semantic.dc_target",
                "'" + sweep.source.text + "' is not a V or I source (only sources and"
                " 'temp' are sweepable in v1)");
            d.loc = sweep.source.loc;
            return spice_fail(std::move(d));
        }
        plan.unit = dev->kind() == 'V' ? 'V' : 'A';
        return plan;
    }
    Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.dc_target",
                            "unknown source '" + sweep.source.text + "' in .dc");
    d.loc = sweep.source.loc;
    return spice_fail(std::move(d));
}

std::string sweep_context(const SweepPlan& inner, double inner_value,
                          const std::optional<SweepPlan>& outer, double outer_value) {
    std::string ctx = ".dc " + inner.source.text + " at " + fmt_value(inner_value) +
                      " " + inner.unit;
    if (outer) {
        ctx += ", " + outer->source.text + " at " + fmt_value(outer_value) + " " +
               outer->unit;
    }
    return ctx;
}

ws::core::Result<Table> run_dc(const DeckAst& ast, const DcCard& card,
                               const RunSettings& settings,
                               std::vector<Warning>& warnings, AnalysisReport& report) {
    // Global .param scope for the sweep expressions.
    ParamTable global;
    for (const ParamDef& def : ast.params) {
        global.define(def);
    }

    WS_TRY_ASSIGN(Circuit circuit, elaborate_deck(ast, kDefaultTemp, warnings));
    if (circuit.devices.empty()) {
        return empty_circuit_error(".dc");
    }

    WS_TRY_ASSIGN(SweepPlan inner, plan_sweep(card.first, global, circuit));
    std::optional<SweepPlan> outer;
    if (card.second) {
        WS_TRY_ASSIGN(outer, plan_sweep(*card.second, global, circuit));
    }

    Table t;
    t.analysis = AnalysisKind::Dc;
    t.sweep = SweepKind::Swept;
    t.sweep_names.push_back(inner.source.text);
    if (outer) {
        t.sweep_names.push_back(outer->source.text);
    }

    // Per-point state. A temp sweep re-elaborates when its value changes
    // (§7.7); everything else mutates only source values (set_sweep_value)
    // over the shared pattern. Warnings surface from the FIRST
    // elaboration only - re-elaborations are identical by construction.
    std::size_t total_iterations = 0;
    std::size_t total_factorizations = 0;
    std::size_t kernel_factorizations = 0;  // last read from the LIVE kernel
    SaveList save = default_save_list(circuit);
    std::unique_ptr<DcKernel> kernel;
    std::vector<double> x;  // warm start across points (§7.3)
    bool first_point = true;
    double last_temp = 0.0;
    bool have_temp = false;
    bool elaborated_with_warnings = true;

    // Builds (or rebuilds) the kernel at `temp`. A temp change
    // re-elaborates at the NEW temperature - including the FIRST point of
    // a temp sweep: the pre-loop elaboration above ran at the §7.7 default
    // (27 C), not the sweep start, so the initial circuit is not reusable
    // there. Warnings were already collected by that first elaboration
    // (re-elaborations are identical modulo temperature), so theirs go to
    // scratch. A new kernel's factorization counter starts at zero, so the
    // accumulator baseline resets with it.
    auto ensure_kernel = [&](bool temp_changed, double temp) -> ws::core::Status {
        if (kernel && !temp_changed) {
            return {};
        }
        if (temp_changed || !elaborated_with_warnings) {
            std::vector<Warning> scratch;
            WS_TRY_ASSIGN(circuit, elaborate_deck(ast, temp, scratch));
        }
        elaborated_with_warnings = false;
        if (circuit.devices.empty()) {
            return empty_circuit_error(".dc");
        }
        save = default_save_list(circuit);
        kernel_factorizations = 0;
        kernel = std::make_unique<DcKernel>(circuit, settings);
        return kernel->prepare();
    };

    const std::size_t outer_count = outer ? outer->count : 1;
    for (std::size_t o = 0; o < outer_count; ++o) {
        const double outer_value = outer ? outer->start + static_cast<double>(o) * outer->step
                                         : 0.0;
        for (std::size_t i = 0; i < inner.count; ++i) {
            const double inner_value =
                inner.start + static_cast<double>(i) * inner.step;

            // Apply the point: temp re-elaborates on change, sources patch.
            double point_temp = kDefaultTemp;
            if (outer && outer->is_temp) {
                point_temp = outer_value;
            }
            if (inner.is_temp) {
                point_temp = inner_value;  // innermost wins if both are temp
            }
            bool temp_changed = false;
            if (inner.is_temp) {
                temp_changed = !have_temp || inner_value != last_temp;
            }
            if (outer && outer->is_temp) {
                temp_changed = temp_changed || !have_temp || outer_value != last_temp;
            }
            if (inner.is_temp || (outer && outer->is_temp)) {
                last_temp = point_temp;
                have_temp = true;
            }
            WS_TRY(ensure_kernel(temp_changed, point_temp));

            for (const SweepPlan* plan : {&inner, outer ? &*outer : nullptr}) {
                if (plan == nullptr || plan->is_temp) {
                    continue;
                }
                const std::string lower = to_lower(plan->source.text);
                bool patched = false;
                for (const auto& dev : circuit.devices) {
                    if (to_lower(dev->name()) == lower) {
                        patched = dev->set_sweep_value(plan == &inner ? inner_value
                                                                      : outer_value);
                        break;
                    }
                }
                if (!patched) {
                    Diagnostic d = run_diag(
                        ErrorKind::Semantic, "spice.semantic.dc_target",
                        "cannot sweep '" + plan->source.text + "' (not a source)");
                    d.analysis = sweep_context(inner, inner_value, outer, outer_value);
                    return spice_fail(std::move(d));
                }
            }

            const std::string ctx =
                sweep_context(inner, inner_value, outer, outer_value);
            const int cap = first_point ? settings.itl1 : settings.itl2;
            std::size_t iterations = 0;
            if (const ws::core::Status st = kernel->solve(x, cap, ctx, iterations);
                !st) {
                return wrap_solver_error(st, ctx);
            }
            total_iterations += iterations;
            // Accumulate the DELTA since the last read: a temp change
            // replaces the kernel, whose counter restarts at zero - reading
            // the raw count here would drop every factorization before the
            // last re-elaboration.
            total_factorizations += kernel->factorizations() - kernel_factorizations;
            kernel_factorizations = kernel->factorizations();
            first_point = false;

            std::vector<double> row;
            row.reserve(t.sweep_names.size() + save.columns.size());
            // Sweep values in sweep_names order: inner first, then outer.
            row.push_back(inner_value);
            if (outer) {
                row.push_back(outer_value);
            }
            for (const double v : extract_row(circuit, save, x)) {
                row.push_back(v);
            }
            t.rows.push_back(std::move(row));
        }
    }

    report.analysis = ".dc";
    report.newton_iterations = total_iterations;
    report.lu_factorizations = total_factorizations;
    report.stage = "newton";
    t.columns = save.columns;
    return t;
}

}  // namespace

// --- the public entry (run.hpp) --------------------------------------------------------

ws::core::Result<RunOutput> run_deck(const DeckAst& ast, RunSettings settings,
                                     std::vector<Warning>& warnings) {
    // .options override the caller's defaults, later cards win (§5.1).
    WS_TRY(apply_options(ast.options, settings));

    RunOutput out;
    for (const AnalysisCard& card : ast.analyses) {
        AnalysisReport report;
        if (card.op) {
            WS_TRY_ASSIGN(Table t, run_op(ast, settings, warnings, report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        } else if (card.dc) {
            WS_TRY_ASSIGN(Table t, run_dc(ast, *card.dc, settings, warnings, report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        }
    }
    if (ast.analyses.empty()) {
        // No analysis cards: nothing to simulate, but the deck still
        // elaborates once - device values and topology are validated, so
        // an invalid deck never passes silently (§1, working-rule 5).
        Circuit circuit;
        WS_TRY_ASSIGN(circuit, elaborate_deck(ast, kDefaultTemp, warnings));
        (void)circuit;
    }
    return out;
}

}  // namespace ws::spice
