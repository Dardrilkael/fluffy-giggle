// Analysis drivers (DESIGN.md §6/§7). Each analysis runs over an
// IMMUTABLE Circuit plus a per-run RunState (§3.5 rev): the kernel owns
// the MNA system, the Newton loop walks the §5.1 convergence rule, and
// every device mutation (companion coefficients, switch positions)
// happens through state slices. Value sweeps (.dc sources) write
// parameter slots; temperature sweeps re-elaborate. The convergence-aid
// ladder (gmin/source stepping, pseudo-transient) lands with M3; every
// M1/M2 device is linear, so plain Newton converges in two iterations
// (solve + confirm) and the ladder paths stay unexercised rather than
// missing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/spice/circuit.hpp>
#include "mna.hpp"

#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>

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

// Appends one sample of every save-list signal from the solution vector
// into the table's columns (columnar fill, §7.8 rev).
void append_row_columns(const Circuit& c, const SaveList& s, Table& t,
                        const std::vector<double>& x) {
    std::size_t k = 0;
    for (const std::size_t n : s.nodes) {
        t.signals[k++].data.push_back(x[n]);
    }
    for (const std::size_t b : s.branches) {
        t.signals[k++].data.push_back(x[c.node_count() + b]);
    }
}

// Signal skeletons from a save-list (names only; data fills per sample).
std::vector<Signal> signal_skeleton(const SaveList& s) {
    std::vector<Signal> out;
    out.reserve(s.columns.size());
    for (const std::string& name : s.columns) {
        Signal sig;
        sig.name = name;
        out.push_back(std::move(sig));
    }
    return out;
}

// Folds every dynamic device's bound parameters into its state slice
// (§3.5b): companion math reads the folded mirrors, so parameters never
// change mid-solve.
void fold_all(const Circuit& c, RunState& rs) {
    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        if (c.devices[i]->is_dynamic()) {
            c.devices[i]->fold_state(rs.state_.data() + c.state_span[i].begin, rs, c);
        }
    }
}

// --- the DC/OP Newton kernel (§5.1) ----------------------------------------------

class DcKernel {
public:
    // The circuit is immutable; every mutation rides the run's state.
    DcKernel(const Circuit& circuit, RunState& state, const RunSettings& settings)
        : circuit_(circuit), state_(state), settings_(settings),
          mna_(circuit.node_count(), circuit.branch_count, circuit) {}

    // Pattern dry run (dc + tran + ac union) + symbolic analyze (§3.2).
    ws::core::Status prepare() { return mna_.setup(circuit_.devices); }

    // The complex twin over the same pattern (§7.4).
    ws::core::Status prepare_ac() { return mna_.setup_ac(); }

    // One Newton solve to convergence. `x` is the warm start in, the
    // solution out. `context` rides into any Diagnostic (§9.2), e.g.
    // ".dc V1 at 3.3 V". `forces` (optional) stamps §6.5 .ic constraints
    // (a stiff Norton per forced node) on every load pass.
    ws::core::Status solve(std::vector<double>& x, int max_iterations,
                           const std::string& context, std::size_t& iterations,
                           const std::vector<std::pair<std::size_t, double>>* forces =
                               nullptr) {
        const std::size_t nc = circuit_.node_count();
        double gforce = 0.0;
        if (forces != nullptr && !forces->empty()) {
            mna_.load(circuit_.devices, state_);
            gforce = 1e9 * std::max(mna_.max_diagonal(), 1e-9);
        }
        // Cold start: an unsized (or stale-sized) x means all zeros.
        if (x.size() != mna_.dim()) {
            x.assign(mna_.dim(), 0.0);
        }
        for (int it = 1; it <= max_iterations; ++it) {
            ++iterations;
            mna_.load(circuit_.devices, state_);
            if (forces != nullptr) {
                for (const auto& [node, volts] : *forces) {
                    mna_.force_node(node, volts, gforce);
                }
            }
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

            // Switch controls re-evaluate against the candidate; a flip
            // means the linearization changed - iterate again.
            bool state_changed = false;
            for (std::size_t i = 0; i < circuit_.devices.size(); ++i) {
                state_changed = circuit_.devices[i]->update_state(
                                    x_new, nc, state_.state_.data() +
                                                   circuit_.state_span[i].begin) ||
                                state_changed;
            }

            // §5.1: |dx| <= reltol*max(|new|,|old|) + tol_abs per unknown,
            // tol_abs = vntol on voltages, abstol on branch currents.
            bool converged = !state_changed;
            if (converged) {
                for (std::size_t i = 0; i < x_new.size(); ++i) {
                    const double tol = settings_.reltol *
                                           std::max(std::abs(x_new[i]), std::abs(x[i])) +
                                       (i < nc ? settings_.vntol : settings_.abstol);
                    if (std::abs(x_new[i] - x[i]) > tol) {
                        converged = false;
                        break;
                    }
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

    // One transient step: companion load (coefficients from the devices'
    // begin_step over their state slices) -> solve -> switch re-evaluation,
    // iterating while a switch flips. Every M2 device is linear, so one
    // solve is exact; the loop exists for the switches only.
    ws::core::Status solve_tran(std::vector<double>& x, const TranStep& step,
                                const std::string& context, std::size_t& iterations) {
        const std::size_t nc = circuit_.node_count();
        for (int it = 1; it <= settings_.itl4; ++it) {
            ++iterations;
            mna_.load_tran(circuit_.devices, state_, step);
            std::vector<double> x_new;
            WS_TRY(mna_.solve(x_new));
            for (const double v : x_new) {
                if (!std::isfinite(v)) {
                    Diagnostic d = run_diag(
                        ErrorKind::Convergence, "spice.convergence.nan",
                        "transient iterate is not finite");
                    d.analysis = context;
                    return spice_fail(std::move(d));
                }
            }
            bool state_changed = false;
            for (std::size_t i = 0; i < circuit_.devices.size(); ++i) {
                state_changed = circuit_.devices[i]->update_state(
                                    x_new, nc, state_.state_.data() +
                                                   circuit_.state_span[i].begin) ||
                                state_changed;
            }
            x = std::move(x_new);
            if (!state_changed) {
                return {};
            }
        }
        Diagnostic d = run_diag(
            ErrorKind::Convergence, "spice.convergence.switch",
            "switch state did not settle after " + std::to_string(settings_.itl4) +
                " solves (chattering control?)");
        d.analysis = context;
        return spice_fail(std::move(d));
    }

    // One .ac point: complex load at omega + solve.
    ws::core::Status solve_ac(std::vector<std::complex<double>>& xc, double omega,
                              const std::string& context) {
        mna_.load_ac(circuit_.devices, state_, omega);
        WS_TRY(mna_.solve_ac(xc));
        for (const std::complex<double>& v : xc) {
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) {
                Diagnostic d = run_diag(
                    ErrorKind::Convergence, "spice.convergence.nan",
                    "small-signal solution is not finite");
                d.analysis = context;
                return spice_fail(std::move(d));
            }
        }
        return {};
    }

    [[nodiscard]] std::size_t factorizations() const { return mna_.factorization_count(); }

private:
    const Circuit& circuit_;
    RunState& state_;
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

ws::core::Result<Table> run_op(const Circuit& circuit, RunState& state,
                               const RunSettings& settings, AnalysisReport& report) {
    if (circuit.devices.empty()) {
        return empty_circuit_error(".op");
    }

    DcKernel kernel(circuit, state, settings);
    if (const ws::core::Status st = kernel.prepare(); !st) {
        return wrap_solver_error(st, ".op");
    }

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
    const SaveList save = default_save_list(circuit);
    t.signals = signal_skeleton(save);
    append_row_columns(circuit, save, t, x);
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

// A .dc grid is materialized row by row; anything past this is almost
// certainly a typo (start/stop/step units mismatch), not a real sweep.
constexpr std::size_t kMaxSweepPoints = 1'000'000;

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

    auto range_fail = [&](std::string message) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.dc_range",
                                std::move(message));
        d.loc = sweep.source.loc;
        return spice_fail(std::move(d));
    };

    // The expression evaluator already rejects NaN/inf results, but the
    // arithmetic below can still overflow (stop-start, or the interval
    // count). Guard both before the size_t cast - an out-of-range double
    // cast to size_t is UB, and a huge count would loop forever.
    if (!std::isfinite(plan.start) || !std::isfinite(plan.stop) ||
        !std::isfinite(plan.step)) {
        return range_fail("sweep values for '" + sweep.source.text +
                          "' must be finite");
    }

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
    const double span = plan.stop - plan.start;
    const double intervals = std::floor(span / plan.step + 1e-9);
    if (!std::isfinite(span) || !std::isfinite(intervals) || intervals < 0.0) {
        return range_fail("sweep range for '" + sweep.source.text +
                          "' is out of range (start " + fmt_value(plan.start) +
                          ", stop " + fmt_value(plan.stop) + ", step " +
                          fmt_value(plan.step) + ")");
    }
    const double points = intervals + 1.0;
    if (!std::isfinite(points) || points < 1.0 ||
        points > static_cast<double>(kMaxSweepPoints)) {
        return range_fail("sweep for '" + sweep.source.text + "' produces " +
                          fmt_value(points) + " points; the maximum is " +
                          std::to_string(kMaxSweepPoints));
    }
    plan.count = static_cast<std::size_t>(points);

    // Classify the target: temp re-elaborates per point (§7.7); anything
    // else must name a V or I source in the circuit.
    const std::string lower = to_lower(sweep.source.text);
    if (lower == "temp") {
        plan.is_temp = true;
        plan.unit = 'C';
        return plan;
    }
    for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
        if (to_lower(circuit.devices[i]->name()) != lower) {
            continue;
        }
        if (circuit.devices[i]->kind() != 'V' && circuit.devices[i]->kind() != 'I') {
            Diagnostic d = run_diag(
                ErrorKind::Semantic, "spice.semantic.dc_target",
                "'" + sweep.source.text + "' is not a V or I source (only sources and"
                " 'temp' are sweepable in v1)");
            d.loc = sweep.source.loc;
            return spice_fail(std::move(d));
        }
        plan.unit = circuit.devices[i]->kind() == 'V' ? 'V' : 'A';
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

ws::core::Result<Table> run_dc(const DeckAst& ast, const Circuit& base_circuit,
                               const DcCard& card, const RunSettings& settings,
                               std::vector<Warning>& warnings, AnalysisReport& report) {
    // Warnings arrive via the caller's single elaboration; temp
    // re-elaborations inside this driver are identical by construction and
    // report to scratch (the §7.7 flow).
    (void)warnings;
    // Global .param scope for the sweep expressions. ParamTable::define()
    // is infallible by design (a later definition replaces an earlier one),
    // so there is no error to drop here.
    ParamTable global;
    for (const ParamDef& def : ast.params) {
        global.define(def);
    }

    if (base_circuit.devices.empty()) {
        return empty_circuit_error(".dc");
    }

    WS_TRY_ASSIGN(SweepPlan inner, plan_sweep(card.first, global, base_circuit));
    std::optional<SweepPlan> outer;
    if (card.second) {
        WS_TRY_ASSIGN(outer, plan_sweep(*card.second, global, base_circuit));
    }

    // Two temp sweeps would share one temperature unknown: the innermost
    // value wins, so the outer temperature silently never changes. That is
    // not a meaningful nested sweep - reject it instead of guessing.
    if (outer && inner.is_temp && outer->is_temp) {
        Diagnostic d = run_diag(
            ErrorKind::Semantic, "spice.semantic.dc_nested_temp",
            "both .dc sweeps target 'temp'; a temperature sweep cannot nest"
            " another temperature sweep");
        d.loc = outer->source.loc;
        return spice_fail(std::move(d));
    }
    // Sweeping the same source twice is equally meaningless: the outer
    // assignment overwrites the inner one, but the table would still show
    // two sweep columns for the same value.
    if (outer && to_lower(inner.source.text) == to_lower(outer->source.text)) {
        Diagnostic d = run_diag(
            ErrorKind::Semantic, "spice.semantic.dc_duplicate_source",
            "both .dc sweeps target '" + inner.source.text +
                "'; each sweep needs a distinct source");
        d.loc = outer->source.loc;
        return spice_fail(std::move(d));
    }

    Table t;
    t.analysis = AnalysisKind::Dc;
    t.sweep = SweepKind::Swept;
    t.sweep_names.push_back(inner.source.text);
    if (outer) {
        t.sweep_names.push_back(outer->source.text);
    }
    t.sweep_axes.assign(t.sweep_names.size(), {});

    // Per-point state. A temp sweep re-elaborates when its value changes
    // (§7.7); everything else mutates only source parameter slots over
    // the shared pattern - the §7.6-rev reuse guarantee: a value write
    // can never move the stamp. Warnings surface from the FIRST
    // elaboration only - re-elaborations are identical by construction.
    std::size_t total_iterations = 0;
    std::size_t total_factorizations = 0;
    std::size_t kernel_factorizations = 0;  // last read from the LIVE kernel
    SaveList save = default_save_list(base_circuit);
    t.signals = signal_skeleton(save);
    std::unique_ptr<DcKernel> kernel;
    std::vector<double> x;  // warm start across points (§7.3)
    bool first_point = true;
    double last_temp = 0.0;
    bool have_temp = false;

    // The live circuit: the caller's (immutable) elaboration, or a local
    // re-elaboration when a temp point demands one. A Circuit is not
    // copyable - and never needs to be: value changes ride RunState, so
    // only temperature (an elaboration input, §7.7) swaps the circuit.
    std::optional<Circuit> temp_circuit;
    const Circuit* live = &base_circuit;
    std::optional<RunState> state;

    // Builds (or rebuilds) the kernel at `temp`. A temp change
    // re-elaborates at the NEW temperature - including the FIRST point of
    // a temp sweep: the caller's circuit was elaborated at the §7.7
    // default (27 C), not the sweep start, so it is not reusable there.
    // Warnings were already collected by that first elaboration
    // (re-elaborations are identical modulo temperature), so theirs go to
    // scratch. A new kernel's factorization counter starts at zero, so the
    // accumulator baseline resets with it.
    auto ensure_kernel = [&](bool temp_changed, double temp) -> ws::core::Status {
        if (kernel && !temp_changed) {
            return {};
        }
        kernel.reset();  // release the old circuit reference before a swap
        if (temp_changed) {
            // §7.7: re-elaborate at the point's temperature. Identical
            // modulo temperature by construction, so warnings go to
            // scratch (the caller already saw the first elaboration's).
            std::vector<Warning> scratch;
            WS_TRY_ASSIGN(*temp_circuit, elaborate_deck(ast, temp, scratch));
            WS_TRY(finalize_circuit(*temp_circuit));
            live = &*temp_circuit;
        }
        const Circuit& circuit = *live;
        if (circuit.devices.empty()) {
            return empty_circuit_error(".dc");
        }
        // A temp re-elaboration yields the same node/device layout by
        // construction, so the columns (and the samples already collected)
        // survive; a genuinely different save list rebuilds the skeleton.
        SaveList fresh = default_save_list(circuit);
        if (fresh.columns != save.columns) {
            save = std::move(fresh);
            t.signals = signal_skeleton(save);
        }
        kernel_factorizations = 0;
        WS_TRY_ASSIGN(*state, RunState::bind(circuit));
        kernel = std::make_unique<DcKernel>(circuit, *state, settings);
        if (const ws::core::Status st = kernel->prepare(); !st) {
            return wrap_solver_error(st, ".dc");
        }
        return {};
    };

    const std::size_t outer_count = outer ? outer->count : 1;
    for (std::size_t o = 0; o < outer_count; ++o) {
        const double outer_value = outer ? outer->start + static_cast<double>(o) * outer->step
                                         : 0.0;
        for (std::size_t i = 0; i < inner.count; ++i) {
            const double inner_value =
                inner.start + static_cast<double>(i) * inner.step;

            // Apply the point: temp re-elaborates on change, sources
            // patch their dc parameter slot.
            double point_temp = settings.temp;
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
            const Circuit& circuit = *live;

            for (const SweepPlan* plan : {&inner, outer ? &*outer : nullptr}) {
                if (plan == nullptr || plan->is_temp) {
                    continue;
                }
                const std::string lower = to_lower(plan->source.text);
                bool patched = false;
                for (std::size_t di = 0; di < circuit.devices.size(); ++di) {
                    if (to_lower(circuit.devices[di]->name()) == lower &&
                        circuit.devices[di]->kind() != 'X' &&
                        circuit.param_span[di].count > 0) {
                        // Source parameter slot 0 is 'dc' (§3.5a).
                        state->params_[circuit.param_span[di].begin] =
                            plan == &inner ? inner_value : outer_value;
                        patched = true;
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

            // Sweep values in sweep_names order: inner first, then outer.
            t.sweep_axes[0].push_back(inner_value);
            if (outer) {
                t.sweep_axes[1].push_back(outer_value);
            }
            append_row_columns(circuit, save, t, x);
        }
    }

    report.analysis = ".dc";
    report.newton_iterations = total_iterations;
    report.lu_factorizations = total_factorizations;
    report.stage = "newton";
    return t;
}

// --- .tran (§6) ----------------------------------------------------------------------

constexpr double kTranInf = std::numeric_limits<double>::infinity();

ws::core::Result<Table> run_tran(const Circuit& circuit, RunState& state,
                                 const TranCard& card, const RunSettings& settings,
                                 const ParamTable& scope,
                                 const std::vector<IcCard>& ics,
                                 AnalysisReport& report) {
    if (circuit.devices.empty()) {
        return empty_circuit_error(".tran");
    }

    // --- resolve and validate the card ------------------------------------
    WS_TRY_ASSIGN(const double tstep, eval_sweep_expr(card.tstep, scope));
    WS_TRY_ASSIGN(const double tstop, eval_sweep_expr(card.tstop, scope));
    double tstart = 0.0;
    if (card.has_tstart) {
        WS_TRY_ASSIGN(tstart, eval_sweep_expr(card.tstart, scope));
    }
    double tmax = 0.0;
    if (card.has_tmax) {
        WS_TRY_ASSIGN(tmax, eval_sweep_expr(card.tmax, scope));
    }
    auto tran_fail = [&](std::string message) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.tran", std::move(message));
        d.loc = card.loc;
        return spice_fail(std::move(d));
    };
    if (!std::isfinite(tstep) || tstep <= 0.0) {
        return tran_fail(".tran tstep must be positive");
    }
    if (!std::isfinite(tstop) || !std::isfinite(tstart) || tstop <= tstart || tstart < 0.0) {
        return tran_fail(".tran needs 0 <= tstart < tstop");
    }
    if (card.has_tmax && (!std::isfinite(tmax) || tmax <= 0.0)) {
        return tran_fail(".tran tmax must be positive");
    }
    // §6.3: dtmax = min(TMAX from the .tran card, (tstop-tstart)/50) - a
    // CONSTANT, not (tstop-t)/50 (which would shrink toward zero at the
    // end). TMAX omitted defaults to TSTEP (ngspice's default, pinned by
    // the §10.1 RC row: with SPICE3's bare (tstop-tstart)/50 the 1-tau
    // point misses the 1e-3*V gate on ordinary decks).
    const double tmax_eff = card.has_tmax ? tmax : tstep;
    const double dtmax = std::min(tmax_eff, (tstop - tstart) / 50.0);
    const double dtmin = std::max(1e-18, tstop * 1e-15);

    DcKernel kernel(circuit, state, settings);
    if (const ws::core::Status st = kernel.prepare(); !st) {
        return wrap_solver_error(st, ".tran");
    }

    // --- .ic: node values (§6.5) ------------------------------------------
    std::vector<std::pair<std::size_t, double>> ic_forces;
    for (const IcCard& ic : ics) {
        for (const auto& [node_tok, expr] : ic.node_values) {
            WS_TRY_ASSIGN(const double volts, eval_sweep_expr(expr, scope));
            const std::string node_lower = to_lower(node_tok.text);
            if (node_lower == "0" || node_lower == "gnd") {
                Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.ic",
                                        "cannot force the ground node");
                d.loc = node_tok.loc;
                return spice_fail(std::move(d));
            }
            const auto it = circuit.node_ids.find(to_lower(node_tok.text));
            if (it == circuit.node_ids.end()) {
                Diagnostic d = run_diag(
                    ErrorKind::Semantic, "spice.semantic.ic",
                    ".ic names unknown node '" + node_tok.text + "'");
                d.loc = node_tok.loc;
                d.hint = "an .ic node must appear on a device card";
                return spice_fail(std::move(d));
            }
            ic_forces.emplace_back(it->second, volts);
        }
    }

    const std::size_t nc = circuit.node_count();
    std::vector<double> x;
    std::size_t total_iterations = 0;

    // --- the t=0 state (§6.5) ----------------------------------------------
    fold_all(circuit, state);
    if (card.uic) {
        // UIC: no OP - .ic node values and device ic= overrides ARE the
        // state; everything else starts at zero.
        x.assign(circuit.dim(), 0.0);
        for (const auto& [node, volts] : ic_forces) {
            x[node] = volts;
        }
        for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
            const Device& dev = *circuit.devices[i];
            if (dev.kind() == 'L' && dev.has_branch() && dev.has_ic()) {
                x[nc + dev.branch()] = dev.ic_value();
            }
        }
        for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
            circuit.devices[i]->seed_state(x, nc,
                                           state.state_.data() +
                                               circuit.state_span[i].begin);
        }
    } else {
        // OP at t=0 with .ic enforced (stiff Norton, released when the
        // transient starts).
        std::size_t iterations = 0;
        if (const ws::core::Status st =
                kernel.solve(x, settings.itl1, ".tran t=0 op", iterations,
                             ic_forces.empty() ? nullptr : &ic_forces);
            !st) {
            return wrap_solver_error(st, ".tran t=0 op");
        }
        total_iterations += iterations;
        for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
            circuit.devices[i]->seed_state(x, nc,
                                           state.state_.data() +
                                               circuit.state_span[i].begin);
        }
    }

    // --- the save-list + the t=0 row ----------------------------------------
    SaveList save = default_save_list(circuit);
    Table t;
    t.analysis = AnalysisKind::Tran;
    t.sweep = SweepKind::Time;
    t.sweep_names.push_back("time");
    t.sweep_axes.assign(1, {});
    t.signals = signal_skeleton(save);
    auto push_row = [&](double now) {
        t.sweep_axes[0].push_back(now);
        append_row_columns(circuit, save, t, x);
    };
    if (tstart <= 0.0) {
        push_row(0.0);
    }

    // --- the time loop (§6.1/§6.3/§6.4) ------------------------------------
    const bool fixed = settings.fixed_dt > 0.0;
    double dt = fixed ? settings.fixed_dt : std::min(tstop / 50.0, dtmax);
    double now = 0.0;  // simulation time (output gated at tstart)
    bool be_next = true;  // §6.1: the first step after t=0 is BE
    bool be_heuristic = false;  // §6.1 ringing damp: one BE step
    int consecutive_rejects = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    while (now < tstop - 1e-12 * std::max(1.0, tstop)) {
        // The next source event strictly after now (§6.4); dt truncates to
        // land on it exactly, and the step AFTER the landing is BE.
        double next_bp = kTranInf;
        for (const auto& dev : circuit.devices) {
            const double e = dev->next_event(now + dtmin, tstep, tstop);
            if (e < next_bp) {
                next_bp = e;
            }
        }
        double step_dt = std::min(dt, tstop - now);
        bool lands_bp = false;
        if (next_bp < kTranInf && next_bp - now < step_dt) {
            step_dt = next_bp - now;
            lands_bp = true;
        }
        if (!(step_dt > 0.0) || step_dt < dtmin) {
            step_dt = std::max(step_dt, dtmin);
        }

        const bool be = fixed ? false : (be_next || be_heuristic);
        const TranStep step{now + step_dt, step_dt, be, tstep, tstop};

        // Parameters never change mid-solve: fold once per attempt, then
        // compute the companion coefficients (§3.5b).
        fold_all(circuit, state);
        for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
            if (circuit.devices[i]->is_dynamic()) {
                circuit.devices[i]->begin_step(
                    step, state.state_.data() + circuit.state_span[i].begin);
            }
        }

        std::size_t iterations = 0;
        {
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), ".tran t=%.6g s", step.time);
            if (const ws::core::Status st = kernel.solve_tran(x, step, ctx, iterations);
                !st) {
                return wrap_solver_error(st, ctx);
            }
        }
        total_iterations += iterations;

        // LTE (§6.3) - skipped entirely in the fixed-step test mode.
        double worst_ratio = 0.0;
        if (!fixed) {
            bool ok = true;
            double shrink_factor = kTranInf;
            for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
                if (!circuit.devices[i]->is_dynamic()) {
                    continue;
                }
                double ratio = 0.0;
                if (!circuit.devices[i]->lte_ok(
                        step, x, nc, settings.reltol, settings.chgtol, settings.trtol,
                        state.state_.data() + circuit.state_span[i].begin, &ratio)) {
                    ok = false;
                    // dt *= min(2, (tolq/eps)^(1/3)) per the violating device.
                    const double f = (ratio > 0.0)
                                         ? std::min(2.0, std::pow(1.0 / ratio, 1.0 / 3.0))
                                         : 0.5;
                    shrink_factor = std::min(shrink_factor, f);
                }
                worst_ratio = std::max(worst_ratio, ratio);
            }
            if (!ok) {
                ++rejected;
                ++consecutive_rejects;
                // §6.1: LTE estimates alternating while dt already shrinks
                // -> one BE step to damp the ringing, then back to trap.
                be_heuristic = (consecutive_rejects >= 3);
                if (shrink_factor < kTranInf) {
                    dt *= shrink_factor;
                }
                if (!std::isfinite(dt) || dt < dtmin) {
                    Diagnostic d = run_diag(
                        ErrorKind::Timestep, "spice.timestep.too_small",
                        "timestep fell below " + fmt_value(dtmin) +
                            " s at t = " + fmt_value(now) +
                            " s; the circuit cannot be resolved (stiff source"
                            " edge or chattering switch?)");
                    d.analysis = ".tran";
                    d.hint = "tighten the source edges, raise chgtol/reltol, or add"
                             " parasitic resistance";
                    return spice_fail(std::move(d));
                }
                continue;  // retry; state is untouched until commit
            }
        }

        // Accept.
        now = step.time;
        for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
            if (circuit.devices[i]->is_dynamic()) {
                circuit.devices[i]->commit(x, nc, step,
                                           state.state_.data() +
                                               circuit.state_span[i].begin);
            }
        }
        ++accepted;
        consecutive_rejects = 0;
        be_heuristic = false;
        be_next = lands_bp;  // §6.1: BE restart after a breakpoint landing

        if (!fixed) {
            // §6.3: growth never exceeds 2x; on accept ratio <= 1 so
            // (tolq/eps)^(1/3) >= 1 grows the step within that cap.
            double growth = 2.0;
            if (worst_ratio > 0.0) {
                growth = std::min(2.0, std::pow(1.0 / worst_ratio, 1.0 / 3.0));
                if (growth < 1.0) {
                    growth = 1.0;
                }
            }
            dt = std::min(dt * growth, dtmax);
        }

        if (now >= tstart - 1e-12 * std::max(1.0, tstart)) {
            push_row(now);
        }
    }

    report.analysis = ".tran";
    report.newton_iterations = total_iterations;
    report.lu_factorizations = kernel.factorizations();
    report.stage = fixed ? "trap(fixed)" : "trap";
    report.accepted_steps = accepted;
    report.rejected_steps = rejected;
    return t;
}

// --- .ac (§7.4) ----------------------------------------------------------------------

ws::core::Result<std::vector<double>> ac_frequency_list(const AcCard& card,
                                                        const ParamTable& params) {
    WS_TRY_ASSIGN(const double n, eval_sweep_expr(card.n, params));
    WS_TRY_ASSIGN(const double fstart, eval_sweep_expr(card.fstart, params));
    WS_TRY_ASSIGN(const double fstop, eval_sweep_expr(card.fstop, params));

    auto ac_fail = [&](std::string message) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.ac",
                                std::move(message));
        d.loc = card.loc;
        return spice_fail(std::move(d));
    };
    if (!std::isfinite(n) || n < 1.0 || std::floor(n) != n) {
        return ac_fail(".ac N must be a positive integer");
    }
    if (!std::isfinite(fstart) || fstart <= 0.0 || !std::isfinite(fstop) || fstop < fstart) {
        return ac_fail(".ac needs 0 < fstart <= fstop");
    }

    std::vector<double> freqs;
    if (card.sweep == AcCard::Sweep::Lin) {
        const std::size_t count = static_cast<std::size_t>(n);
        if (count > kMaxSweepPoints) {
            return ac_fail(".ac produces too many points");
        }
        freqs.reserve(count);
        if (count == 1) {
            freqs.push_back(fstart);
        } else {
            for (std::size_t k = 0; k < count; ++k) {
                // Direct computation, never repeated multiplication (§7.4).
                freqs.push_back(fstart + (fstop - fstart) *
                                             static_cast<double>(k) /
                                             static_cast<double>(count - 1));
            }
        }
        return freqs;
    }
    // dec/oct: f_k = fstart * base^(k/N), k = 0..floor(N*log_base(fstop/fstart)+1e-9)
    const double base = (card.sweep == AcCard::Sweep::Dec) ? 10.0 : 2.0;
    const double log_ratio =
        (card.sweep == AcCard::Sweep::Dec) ? std::log10(fstop / fstart)
                                           : std::log2(fstop / fstart);
    const double kd = std::floor(n * log_ratio + 1e-9) + 1.0;
    if (!std::isfinite(kd) || kd < 1.0 || kd > static_cast<double>(kMaxSweepPoints)) {
        return ac_fail(".ac produces too many points");
    }
    const std::size_t count = static_cast<std::size_t>(kd);
    freqs.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        freqs.push_back(fstart * std::pow(base, static_cast<double>(k) / n));
    }
    // Grid roundoff may leave the last point a hair above fstop; the
    // endpoint is IN when it lands on the grid (§7.4), never above it.
    if (!freqs.empty() && freqs.back() > fstop) {
        freqs.back() = fstop;
    }
    return freqs;
}

ws::core::Result<Table> run_ac(const Circuit& circuit, RunState& state,
                               const AcCard& card, const RunSettings& settings,
                               const ParamTable& scope, AnalysisReport& report) {
    if (circuit.devices.empty()) {
        return empty_circuit_error(".ac");
    }

    WS_TRY_ASSIGN(const std::vector<double> freqs, ac_frequency_list(card, scope));

    DcKernel kernel(circuit, state, settings);
    if (const ws::core::Status st = kernel.prepare(); !st) {
        return wrap_solver_error(st, ".ac");
    }

    // §7.1: the OP is this analysis's own (never borrowed), sources at
    // their DC values; switch states freeze wherever it settles.
    {
        std::vector<double> x_op;
        std::size_t iterations = 0;
        if (const ws::core::Status st = kernel.solve(x_op, settings.itl1, ".ac op",
                                                     iterations);
            !st) {
            return wrap_solver_error(st, ".ac op");
        }
        report.newton_iterations = iterations;
    }

    // §7.4: a deck with no AC source is an error, never a table of zeros.
    bool any_ac = false;
    for (const auto& dev : circuit.devices) {
        if (dev->has_ac()) {
            any_ac = true;
            break;
        }
    }
    if (!any_ac) {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.ac_stimulus",
                                "no AC stimulus: no source carries an 'AC magnitude'"
                                " spec");
        d.loc = card.loc;
        d.hint = "write e.g. 'V1 in 0 DC 0 AC 1' and re-run";
        return spice_fail(std::move(d));
    }

    if (const ws::core::Status st = kernel.prepare_ac(); !st) {
        return wrap_solver_error(st, ".ac");
    }

    // Fold the bound parameters into the state slices once: the K-coupled
    // inductors' mutual M recomputes from the CURRENT L values (§3.5b),
    // and the switches' settled positions ride their state slots.
    fold_all(circuit, state);

    // Signals: frequency axis, then re()/im() pairs for the default
    // save-list (the locked CSV decision: re()/im(), never mag/phase).
    const SaveList save = default_save_list(circuit);
    Table t;
    t.analysis = AnalysisKind::Ac;
    t.sweep = SweepKind::Frequency;
    t.sweep_names.push_back("frequency");
    t.sweep_axes.assign(1, {});
    for (const std::string& col : save.columns) {
        Signal re;
        re.name = "re(" + col + ")";
        t.signals.push_back(std::move(re));
        Signal im;
        im.name = "im(" + col + ")";
        t.signals.push_back(std::move(im));
    }

    for (const double f : freqs) {
        std::vector<std::complex<double>> xc;
        {
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), ".ac at %.6g Hz", f);
            if (const ws::core::Status st =
                     kernel.solve_ac(xc, 2.0 * 3.14159265358979323846 * f, ctx);
                !st) {
                return wrap_solver_error(st, ctx);
            }
        }
        t.sweep_axes[0].push_back(f);
        std::size_t k = 0;
        for (const std::size_t n : save.nodes) {
            t.signals[k++].data.push_back(xc[n].real());
            t.signals[k++].data.push_back(xc[n].imag());
        }
        for (const std::size_t b : save.branches) {
            t.signals[k++].data.push_back(xc[circuit.node_count() + b].real());
            t.signals[k++].data.push_back(xc[circuit.node_count() + b].imag());
        }
    }

    report.analysis = ".ac";
    report.lu_factorizations = kernel.factorizations();
    report.stage = "ac";
    return t;
}

}  // namespace

// --- the public entry (run.hpp) --------------------------------------------------------

namespace drivers {

ws::core::Result<Table> run_op(const Circuit& circuit, RunState& state,
                               const RunSettings& settings, AnalysisReport& report) {
    return ::ws::spice::run_op(circuit, state, settings, report);
}

ws::core::Result<Table> run_dc(const DeckAst* ast, const Circuit& base_circuit,
                               const DcCard& card, const RunSettings& settings,
                               std::vector<Warning>& warnings, AnalysisReport& report) {
    // No deck behind this circuit: value sweeps run fine (they only patch
    // parameter slots), but a temperature sweep re-elaborates from the AST
    // (§7.7) - name the limitation instead of guessing.
    if (ast == nullptr) {
        const auto is_temp = [](const DcSweep& sw) {
            return to_lower(sw.source.text) == "temp";
        };
        if (is_temp(card.first) || (card.second && is_temp(*card.second))) {
            Diagnostic d;
            d.kind = ErrorKind::Usage;
            d.code = "spice.usage.dc_temp_no_deck";
            d.message = "a '.dc temp' sweep re-elaborates the deck per point; a"
                        " Simulator-created circuit carries no deck - sweep a"
                        " source value instead";
            d.analysis = ".dc";
            return spice_fail(std::move(d));
        }
        static const DeckAst empty;  // never consulted beyond temp paths
        return ::ws::spice::run_dc(empty, base_circuit, card, settings, warnings, report);
    }
    return ::ws::spice::run_dc(*ast, base_circuit, card, settings, warnings, report);
}

ws::core::Result<Table> run_tran(const Circuit& circuit, RunState& state,
                                  const TranCard& card, const RunSettings& settings,
                                  const ParamTable& scope, const std::vector<IcCard>& ics,
                                  AnalysisReport& report) {
    return ::ws::spice::run_tran(circuit, state, card, settings, scope, ics, report);
}

ws::core::Result<Table> run_ac(const Circuit& circuit, RunState& state,
                               const AcCard& card, const RunSettings& settings,
                               const ParamTable& scope, AnalysisReport& report) {
    return ::ws::spice::run_ac(circuit, state, card, settings, scope, report);
}

}  // namespace drivers

ws::core::Result<RunOutput> run_deck(const DeckAst& ast, RunSettings settings,
                                     std::vector<Warning>& warnings) {
    // .options override the caller's defaults, later cards win (§5.1).
    WS_TRY(apply_options(ast.options, settings));

    // The parser can only build exactly one analysis kind per card, but
    // DeckAst is a public type: a hand-built AST could carry none or
    // several. Reject those up front instead of silently skipping the
    // card (working-rule 5).
    for (const AnalysisCard& card : ast.analyses) {
        const int kinds = (card.op ? 1 : 0) + (card.dc ? 1 : 0) + (card.tran ? 1 : 0) +
                          (card.ac ? 1 : 0);
        if (kinds != 1) {
            Diagnostic d = run_diag(
                ErrorKind::Semantic, "spice.semantic.analysis_card",
                kinds == 0
                    ? "analysis card carries neither .op nor .dc"
                    : "analysis card carries more than one analysis"
                      " (both .op and .dc, say)");
            d.loc = card.loc;
            return spice_fail(std::move(d));
        }
    }

    // One elaboration serves every analysis (§7.1 deck order never
    // changes an answer: each analysis gets a FRESH RunState, so value
    // sweeps and dynamic state cannot leak across). Temperature is the
    // one elaboration input - a `.dc temp` point re-elaborates itself.
    WS_TRY_ASSIGN(Circuit circuit, elaborate_deck(ast, settings.temp, warnings));
    WS_TRY(finalize_circuit(circuit));
    if (circuit.devices.empty() && !ast.analyses.empty()) {
        // The per-analysis drivers report with their own context.
    }

    ParamTable global;
    for (const ParamDef& def : ast.params) {
        global.define(def);
    }

    RunOutput out;
    for (const AnalysisCard& card : ast.analyses) {
        AnalysisReport report;
        // A fresh RunState per analysis: parameters rebind from the deck's
        // scope, state re-seeds. This is the reset that §3.5 rev makes
        // trivial.
        WS_TRY_ASSIGN(RunState state, RunState::bind(circuit));
        if (card.op) {
            WS_TRY_ASSIGN(Table t, run_op(circuit, state, settings, report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        } else if (card.dc) {
            WS_TRY_ASSIGN(Table t, run_dc(ast, circuit, *card.dc, settings, warnings,
                                          report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        } else if (card.tran) {
            WS_TRY_ASSIGN(Table t, run_tran(circuit, state, *card.tran, settings, global,
                                            ast.ics, report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        } else {
            WS_TRY_ASSIGN(Table t, run_ac(circuit, state, *card.ac, settings, global,
                                          report));
            out.results.tables.push_back(std::move(t));
            out.reports.push_back(std::move(report));
        }
    }
    if (ast.analyses.empty()) {
        // No analysis cards: nothing to simulate, but the deck still
        // elaborates once - device values and topology are validated, so
        // an invalid deck never passes silently (§1, working-rule 5).
        if (circuit.devices.empty()) {
            return empty_circuit_error("");
        }
    }
    return out;
}

}  // namespace ws::spice
