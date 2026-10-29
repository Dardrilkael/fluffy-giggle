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
#include <ws/spice/circuit.hpp>
#include "drivers.hpp"
#include "kernel.hpp"
#include "mna.hpp"

#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

namespace {

using ws::spice::detail::DcKernel;
using ws::spice::detail::SaveList;
using ws::spice::detail::fold_all;
using ws::spice::detail::append_row_columns;
using ws::spice::detail::default_save_list;
using ws::spice::detail::signal_skeleton;

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
    report.stage = kernel.stage();  // §5.2: which ladder stage converged

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
    std::string last_stage = "newton";

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
            last_stage = kernel->stage();

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
    report.stage = last_stage;  // §5.2: the worst point's ladder stage
    return t;
}

// --- .tran (§6) ----------------------------------------------------------------------

constexpr double kTranInf = std::numeric_limits<double>::infinity();

ws::core::Result<Table> run_tran(const Circuit& circuit, RunState& state,
                                 const TranCard& card, const RunSettings& settings,
                                 const ParamTable& scope,
                                 const std::vector<IcCard>& ics,
                                 AnalysisReport& report) {
    auto march = std::make_unique<drivers::TranMarch>(circuit, state, settings, card,
                                                      scope, ics);
    WS_TRY(march->prepare());
    const drivers::TranMarch::Hooks no_hooks{};
    while (!march->done() && !march->stopped()) {
        WS_TRY(march->step(no_hooks));
    }
    report = march->report();
    return march->take_table();
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

// --- .step point enumeration (§7.6) ---------------------------------------------

// One point of the (nested) step family: the overridden param values, the
// elaboration temperature (temp steps), the swept source values, and the
// 1-based point number.
struct StepPoint {
    std::vector<std::pair<std::string, double>> params;  // (lowercased name, value)
    std::vector<std::pair<std::string, double>> sources;  // swept source dc values
    double temp = kDefaultTemp;
    bool temp_from_step = false;  // a temp step card set it
    std::size_t number = 1;
    std::string spell;  // "rload=2k, run=3" - error context
};

// Materializes one StepCard's values (lin/list/dec/oct grids, the §7.3
// guards: finite, nonzero step, direction, count cap).
ws::core::Result<std::vector<double>> step_values(const StepCard& card,
                                                  const ParamTable& scope) {
    auto eval = [&](const ExprProgram& p) -> ws::core::Result<double> {
        WS_TRY_ASSIGN(const std::vector<std::optional<double>> b, scope.bind(p));
        WS_TRY_ASSIGN(const double v, p.evaluate(b));
        return v;
    };
    auto fail = [&](std::string m) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.step", std::move(m));
        d.loc = card.loc;
        return spice_fail(std::move(d));
    };
    std::vector<double> out;
    switch (card.kind) {
        case StepCard::Kind::List: {
            for (const ExprProgram& p : card.list) {
                WS_TRY_ASSIGN(const double v, eval(p));
                out.push_back(v);
            }
            return out;
        }
        case StepCard::Kind::Dec:
        case StepCard::Kind::Oct: {
            WS_TRY_ASSIGN(const double start, eval(card.start));
            WS_TRY_ASSIGN(const double stop, eval(card.stop));
            WS_TRY_ASSIGN(const double n, eval(card.inc));  // N in this form
            if (!std::isfinite(start) || !std::isfinite(stop) || !std::isfinite(n) ||
                n < 1.0 || std::floor(n) != n) {
                return fail(".step dec/oct needs a positive integer N");
            }
            if (!(start > 0.0) || !(stop >= start)) {
                return fail(".step dec/oct needs 0 < start <= stop");
            }
            const double base = (card.kind == StepCard::Kind::Dec) ? 10.0 : 2.0;
            const double log_ratio = (card.kind == StepCard::Kind::Dec)
                                         ? std::log10(stop / start)
                                         : std::log2(stop / start);
            const double kd = std::floor(n * log_ratio + 1e-9) + 1.0;
            if (!std::isfinite(kd) || kd < 1.0 ||
                kd > static_cast<double>(kMaxSweepPoints)) {
                return fail(".step produces too many points");
            }
            for (std::size_t k = 0; k < static_cast<std::size_t>(kd); ++k) {
                out.push_back(start * std::pow(base, static_cast<double>(k) / n));
            }
            if (!out.empty() && out.back() > stop) {
                out.back() = stop;
            }
            return out;
        }
        case StepCard::Kind::Lin: {
            WS_TRY_ASSIGN(const double start, eval(card.start));
            WS_TRY_ASSIGN(const double stop, eval(card.stop));
            WS_TRY_ASSIGN(const double inc, eval(card.inc));
            if (!std::isfinite(start) || !std::isfinite(stop) || !std::isfinite(inc)) {
                return fail(".step values must be finite");
            }
            if (inc == 0.0) {
                return fail(".step increment is zero");
            }
            if ((stop > start && inc < 0.0) || (stop < start && inc > 0.0)) {
                return fail(".step increment must point toward stop");
            }
            const double span = stop - start;
            const double intervals = std::floor(span / inc + 1e-9);
            if (!std::isfinite(span) || !std::isfinite(intervals) || intervals < 0.0) {
                return fail(".step range is out of range");
            }
            const double points = intervals + 1.0;
            if (points > static_cast<double>(kMaxSweepPoints)) {
                return fail(".step produces too many points");
            }
            for (std::size_t k = 0; k < static_cast<std::size_t>(points); ++k) {
                out.push_back(start + static_cast<double>(k) * inc);
            }
            return out;
        }
    }
    return fail("internal: unhandled .step kind");
}

// The cartesian product, first card outermost (§7.6).
ws::core::Result<std::vector<StepPoint>> enumerate_steps(
    const std::vector<StepCard>& cards, const ParamTable& scope, double base_temp) {
    std::vector<StepPoint> points(1);  // the empty product
    points[0].temp = base_temp;
    for (const StepCard& card : cards) {
        WS_TRY_ASSIGN(const std::vector<double> values, step_values(card, scope));
        std::vector<StepPoint> next;
        next.reserve(points.size() * values.size());
        for (const StepPoint& p : points) {
            for (const double v : values) {
                StepPoint q = p;
                if (card.is_temp) {
                    q.temp = v;
                    q.temp_from_step = true;
                } else if (card.is_source) {
                    q.sources.emplace_back(to_lower(card.name.text), v);
                } else {
                    q.params.emplace_back(to_lower(card.name.text), v);
                }
                next.push_back(std::move(q));
            }
        }
        points = std::move(next);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        points[i].number = i + 1;
        for (const auto& [name, v] : points[i].params) {
            points[i].spell +=
                (points[i].spell.empty() ? std::string() : std::string(", ")) +
                name + "=" + fmt_value(v);
        }
        for (const auto& [name, v] : points[i].sources) {
            points[i].spell +=
                (points[i].spell.empty() ? std::string() : std::string(", ")) +
                name + "=" + fmt_value(v);
        }
        if (points[i].temp_from_step) {
            points[i].spell +=
                (points[i].spell.empty() ? std::string() : std::string(", ")) +
                "temp=" + fmt_value(points[i].temp);
        }
    }
    return points;
}

// Merges one point's analysis table into the family table: the step axes
// prepend (step number, then each stepped quantity), the analysis's own
// axes and signals append.
void merge_into_family(Table& family, const Table& point, const StepPoint& sp,
                        const std::vector<std::string>& step_names) {
    if (family.signals.empty()) {
        family.analysis = point.analysis;
        family.sweep = point.sweep;
        family.sweep_names.push_back("step");
        for (const std::string& n : step_names) {
            family.sweep_names.push_back(n);
        }
        for (const std::string& n : point.sweep_names) {
            family.sweep_names.push_back(n);
        }
        family.sweep_axes.assign(family.sweep_names.size(), {});
        family.signals = point.signals;  // names
        for (Signal& sig : family.signals) {
            sig.data.clear();
        }
    }
    const std::size_t n = point.row_count();
    for (std::size_t i = 0; i < n; ++i) {
        family.sweep_axes[0].push_back(static_cast<double>(sp.number));
        std::size_t ax = 1;
        for (const auto& [name, v] : sp.params) {
            (void)name;
            family.sweep_axes[ax++].push_back(v);
        }
        for (const auto& [name, v] : sp.sources) {
            (void)name;
            family.sweep_axes[ax++].push_back(v);
        }
        if (sp.temp_from_step) {
            family.sweep_axes[ax++].push_back(sp.temp);
        }
        for (const auto& a : point.sweep_axes) {
            family.sweep_axes[ax++].push_back(i < a.size() ? a[i] : 0.0);
        }
        std::size_t k = 0;
        for (const Signal& sig : point.signals) {
            family.signals[k++].data.push_back(i < sig.data.size() ? sig.data[i] : 0.0);
        }
    }
}

// --- the public entry (run.hpp) --------------------------------------------------------

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

    ParamTable global;
    for (const ParamDef& def : ast.params) {
        global.define(def);
    }
    if (ast.steps.empty()) {
        // Cycle check (§8.4) on the plain scope. A stepped run checks per
        // point (the elaborator's own eager pass, with the point's
        // overrides in scope); here a gauss()/flat() definition defers to
        // run time (§8.5 rev).
        for (const ParamDef& def : ast.params) {
            const ws::core::Result<double> v = global.value(to_lower(def.name.text));
            if (!v && v.error().category == std::string("spice") &&
                v.error().message.find("needs a random context") != std::string::npos) {
                continue;
            }
            WS_TRY(v);
        }
    }

    // The step family (§7.6): the cartesian product of the .step cards,
    // first card outermost. Without .step cards this is the single empty
    // point - the plain run.
    WS_TRY_ASSIGN(const std::vector<StepPoint> points,
                  enumerate_steps(ast.steps, global, settings.temp));

    // Step names for the family axes: the stepped params (then sources,
    // then temp) in card order.
    std::vector<std::string> step_names;
    for (const StepCard& card : ast.steps) {
        step_names.push_back(card.name.text);
    }

    // Elaboration: a stepped run elaborates per point (the stepped params
    // are elaboration-scope values - devices referencing them bake in at
    // the point's number, and a temp step re-elaborates at ITS value,
    // §7.7); a plain run elaborates once.
    std::optional<Circuit> base;
    if (ast.steps.empty()) {
        WS_TRY_ASSIGN(base, elaborate_deck(ast, settings.temp, warnings));
    }

    // The Monte-Carlo source (§8.5 rev): one stream per run, advanced
    // point by point - the recorded seed reproduces the whole family.
    Rand rng(settings.seed);

    RunOutput out;
    out.seed = settings.seed;
    std::vector<Table> family(ast.analyses.size());
    std::vector<std::string> failure_list;
    bool elaborated_any_devices = false;

    for (const StepPoint& sp : points) {
        // The point's scope: global defs + the stepped constants. A
        // re-definition clears that name's resolve cache, so devices and
        // .params referencing the stepped name re-draw correctly.
        ParamTable scope(global);
        for (const auto& [name, v] : sp.params) {
            ParamDef def;
            def.name.text = name;
            def.expr = ExprProgram::constant(v);
            scope.define(std::move(def));
        }

        // The circuit for this point: the plain run's single elaboration,
        // or a per-point elaboration carrying the stepped values.
        Circuit point_circuit;
        if (base.has_value()) {
            point_circuit = std::move(*base);
            base.reset();  // the single plain point consumed it
        } else {
            std::vector<Warning> scratch;  // identical modulo the step values
            ws::core::Result<Circuit> re =
                elaborate_deck(ast, sp.temp, scratch, &sp.params);
            if (!re) {
                Diagnostic d = run_diag(ErrorKind::Semantic,
                                        "spice.semantic.step_elaborate",
                                        "step point {" + sp.spell +
                                            "}: " + re.error().message);
                return spice_fail(std::move(d));
            }
            point_circuit = std::move(*re);
        }
        const Circuit* live = &point_circuit;
        if (live->devices.empty()) {
            return empty_circuit_error("");
        }
        elaborated_any_devices = true;

        // Each analysis binds a FRESH RunState against the point's scope
        // (the §3.5 rev reset) and appends into its family table. A bind
        // failure (a Monte-Carlo draw outside a device's domain, a
        // check_params rejection) carries the step context.
        for (std::size_t ai = 0; ai < ast.analyses.size(); ++ai) {
            const AnalysisCard& card = ast.analyses[ai];
            ws::core::Result<RunState> bound = RunState::bind(*live, &scope, &rng);
            if (!bound) {
                if (ast.steps.empty()) {
                    return ws::core::unexpected(bound.error());
                }
                Diagnostic d = run_diag(
                    ErrorKind::Semantic, "spice.semantic.step_failed",
                    "step point {" + sp.spell + "}: " + bound.error().message);
                if (!settings.keep_going) {
                    return spice_fail(std::move(d));
                }
                failure_list.push_back("step " + std::to_string(sp.number) + " {" +
                                       sp.spell + "}: " + bound.error().message);
                continue;
            }
            RunState state = std::move(*bound);
            // Swept sources patch their dc slot (values only, §3.5 rev).
            for (const auto& [name, v] : sp.sources) {
                if (const auto di = RunState::device_index(*live, name)) {
                    if (live->param_span[*di].count > 0) {
                        state.params_[live->param_span[*di].begin] = v;
                    }
                }
            }
            AnalysisReport report;
            ws::core::Result<Table> t = ws::core::unexpected(
                ws::core::make_error(1, "spice", "internal: no analysis"));
            if (card.op) {
                t = run_op(*live, state, settings, report);
            } else if (card.dc) {
                t = run_dc(ast, *live, *card.dc, settings, warnings, report);
            } else if (card.tran) {
                t = run_tran(*live, state, *card.tran, settings, scope, ast.ics, report);
            } else {
                t = run_ac(*live, state, *card.ac, settings, scope, report);
            }
            if (!t) {
                if (ast.steps.empty()) {
                    // A plain run: the driver's own diagnostic reaches the
                    // caller untouched (the §9 taxonomy, never re-branded).
                    return ws::core::unexpected(t.error());
                }
                // §7.6: abort naming the step point; --keep-going runs the
                // rest and reports the list at the end.
                Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.step_failed",
                                        "step point {" + sp.spell +
                                            "} failed: " + t.error().message);
                if (!settings.keep_going) {
                    return spice_fail(std::move(d));
                }
                failure_list.push_back("step " + std::to_string(sp.number) + " {" +
                                       sp.spell + "}: " + t.error().message);
                continue;
            }
            if (ast.steps.empty()) {
                // No .step cards: the point's table IS the result (no
                // leading step column - the plain §0.5 shape).
                out.results.tables.push_back(std::move(*t));
            } else {
                merge_into_family(family[ai], *t, sp, step_names);
            }
            out.reports.push_back(std::move(report));
        }
    }

    // A stepped run's family tables replace the per-point ones (§7.6: one
    // file holds the whole family, a leading step axis per §7.6).
    if (!ast.steps.empty()) {
        for (std::size_t ai = 0; ai < ast.analyses.size(); ++ai) {
            if (!family[ai].signals.empty()) {
                out.results.tables.push_back(std::move(family[ai]));
            }
        }
    }

    if (!failure_list.empty()) {
        std::string msg = "step run completed with failures:\n";
        for (const std::string& f : failure_list) {
            msg += "  " + f + "\n";
        }
        Diagnostic d = run_diag(ErrorKind::Semantic, "spice.semantic.step_failures",
                                std::move(msg));
        return spice_fail(std::move(d));
    }

    if (ast.analyses.empty()) {
        // No analysis cards: nothing to simulate, but the deck still
        // elaborates once - device values and topology are validated, so
        // an invalid deck never passes silently (§1, working-rule 5).
        // (The step loop above already elaborated every point.)
        if (!elaborated_any_devices) {
            return empty_circuit_error("");
        }
    }
    return out;
}

}  // namespace ws::spice
