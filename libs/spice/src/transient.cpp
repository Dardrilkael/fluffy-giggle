// The steppable transient (DESIGN.md §6 rev): TranMarch is the time loop
// extracted from run_tran - one class, one set of timestep semantics
// (§6.1 BE restarts, §6.3 LTE, §6.4 breakpoints), driven either to
// completion by the plain driver or step-by-step by TransientRun. Hooks
// (stop token, progress callback) are consulted per step attempt, so a
// cancel is prompt and a diverging run can be aborted early by the
// caller's own criterion.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "drivers.hpp"
#include "kernel.hpp"

#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/simulator.hpp>
#include <ws/spice/state.hpp>
#include <ws/spice/transient.hpp>

namespace ws::spice {

namespace {

using detail::DcKernel;
using detail::SaveList;
using detail::append_row_columns;
using detail::default_save_list;
using detail::fold_all;
using detail::run_diag;
using detail::signal_skeleton;

Diagnostic march_diag(ErrorKind kind, std::string code, std::string message) {
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

ws::core::Result<double> eval_expr(const ExprProgram& prog, const ParamTable& params) {
    WS_TRY_ASSIGN(const std::vector<std::optional<double>> bindings, params.bind(prog));
    WS_TRY_ASSIGN(const double v, prog.evaluate(bindings));
    return v;
}

ws::core::unexpected<ws::core::Error> wrap_solver_error(const ws::core::Status& st,
                                                        const std::string& context) {
    if (std::string_view(st.error().category) == "spice") {
        return ws::core::unexpected(st.error());
    }
    Diagnostic d = march_diag(ErrorKind::Singular, "spice.singular",
                              st.error().message.empty() ? "matrix factorization failed"
                                                         : st.error().message);
    d.analysis = context;
    return spice_fail(std::move(d));
}

}  // namespace

namespace drivers {

// --- TranMarch::Impl -------------------------------------------------------------

struct TranMarch::Impl {
    Impl(const Circuit& c, RunState& st, const RunSettings& se, const TranCard& ca,
         const ParamTable& sc, const std::vector<IcCard>& ic)
        : circuit(c), state(st), settings(se), card(ca), scope(sc), ics(ic) {}

    const Circuit& circuit;
    RunState& state;
    const RunSettings& settings;
    TranCard card;
    const ParamTable& scope;
    std::vector<IcCard> ics;

    // Card-resolved constants (prepare()).
    double tstep = 0.0, tstop = 0.0, tstart = 0.0;
    double dtmax = 0.0, dtmin = 0.0;
    bool fixed = false;

    std::unique_ptr<DcKernel> kernel;
    std::vector<double> x;               // the accepted solution
    std::vector<std::pair<std::size_t, double>> ic_forces;

    // The march.
    double dt = 0.0;
    double now = 0.0;
    bool be_next = true;   // §6.1: the first step after t=0 is BE
    bool be_heuristic = false;
    int consecutive_rejects = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t total_iterations = 0;
    bool prepared = false;
    bool stopped = false;

    SaveList save;
    Table table;
    constexpr static double kInf = std::numeric_limits<double>::infinity();

    void push_row(double at) {
        table.sweep_axes[0].push_back(at);
        append_row_columns(circuit, save, table, x);
    }

    ws::core::Status validate_card();
    ws::core::Status seed_t0();
    ws::core::Status one_step(const Hooks& hooks);
};

ws::core::Status TranMarch::Impl::validate_card() {
    WS_TRY_ASSIGN(tstep, eval_expr(card.tstep, scope));
    WS_TRY_ASSIGN(tstop, eval_expr(card.tstop, scope));
    if (card.has_tstart) {
        WS_TRY_ASSIGN(tstart, eval_expr(card.tstart, scope));
    }
    double tmax = 0.0;
    if (card.has_tmax) {
        WS_TRY_ASSIGN(tmax, eval_expr(card.tmax, scope));
    }
    auto tran_fail = [&](std::string message) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d = march_diag(ErrorKind::Semantic, "spice.semantic.tran",
                                  std::move(message));
        d.loc = card.loc;
        return spice_fail(std::move(d));
    };
    if (!std::isfinite(tstep) || tstep <= 0.0) {
        return tran_fail(".tran tstep must be positive");
    }
    if (!std::isfinite(tstop) || !std::isfinite(tstart) || tstop <= tstart ||
        tstart < 0.0) {
        return tran_fail(".tran needs 0 <= tstart < tstop");
    }
    if (card.has_tmax && (!std::isfinite(tmax) || tmax <= 0.0)) {
        return tran_fail(".tran tmax must be positive");
    }
    // §6.3: dtmax = min(TMAX from the .tran card, (tstop-tstart)/50) - a
    // CONSTANT, not (tstop-t)/50. TMAX omitted defaults to TSTEP (ngspice).
    const double tmax_eff = card.has_tmax ? tmax : tstep;
    dtmax = std::min(tmax_eff, (tstop - tstart) / 50.0);
    dtmin = std::max(1e-18, tstop * 1e-15);
    fixed = settings.fixed_dt > 0.0;
    return {};
}

ws::core::Status TranMarch::Impl::seed_t0() {
    // --- .ic: node values (§6.5) ------------------------------------------
    for (const IcCard& ic : ics) {
        for (const auto& [node_tok, expr] : ic.node_values) {
            WS_TRY_ASSIGN(const double volts, eval_expr(expr, scope));
            const std::string node_lower = to_lower(node_tok.text);
            if (node_lower == "0" || node_lower == "gnd") {
                Diagnostic d = march_diag(ErrorKind::Semantic, "spice.semantic.ic",
                                          "cannot force the ground node");
                d.loc = node_tok.loc;
                return spice_fail(std::move(d));
            }
            const auto it = circuit.node_ids.find(to_lower(node_tok.text));
            if (it == circuit.node_ids.end()) {
                Diagnostic d = march_diag(
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
    x.clear();
    total_iterations = 0;

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
                kernel->solve(x, settings.itl1, ".tran t=0 op", iterations,
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
    save = default_save_list(circuit);
    table = Table{};
    table.analysis = AnalysisKind::Tran;
    table.sweep = SweepKind::Time;
    table.sweep_names.push_back("time");
    table.sweep_axes.assign(1, {});
    table.signals = signal_skeleton(save);
    if (tstart <= 0.0) {
        push_row(0.0);
    }

    dt = fixed ? settings.fixed_dt : std::min(tstop / 50.0, dtmax);
    now = 0.0;
    be_next = true;
    be_heuristic = false;
    consecutive_rejects = 0;
    accepted = 0;
    rejected = 0;
    return {};
}

ws::core::Status TranMarch::Impl::one_step(const Hooks& hooks) {
    // --- the time loop body (§6.1/§6.3/§6.4), one accepted step -------------
    while (now < tstop - 1e-12 * std::max(1.0, tstop)) {
        // Cancellation is prompt: checked before every attempt.
        if (hooks.stop != nullptr && hooks.stop->stop_requested()) {
            stopped = true;
            return {};
        }

        // The next source event strictly after now (§6.4); dt truncates to
        // land on it exactly, and the step AFTER the landing is BE.
        double next_bp = kInf;
        for (const auto& dev : circuit.devices) {
            const double e = dev->next_event(now + dtmin, tstep, tstop);
            if (e < next_bp) {
                next_bp = e;
            }
        }
        double step_dt = std::min(dt, tstop - now);
        bool lands_bp = false;
        if (next_bp < kInf && next_bp - now < step_dt) {
            step_dt = next_bp - now;
            lands_bp = true;
        }
        if (!(step_dt > 0.0) || step_dt < dtmin) {
            step_dt = std::max(step_dt, dtmin);
        }

        const bool be = fixed ? false : (be_next || be_heuristic);
        const TranStep step{now + step_dt, step_dt, be, tstep, tstop};

        // Parameters never change mid-solve: fold once per attempt, then
        // compute the companion coefficients (§3.5b). This is also where a
        // set_source() between advance_to() calls takes effect.
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
            if (const ws::core::Status st = kernel->solve_tran(x, step, ctx, iterations);
                !st) {
                return wrap_solver_error(st, ctx);
            }
        }
        total_iterations += iterations;

        // LTE (§6.3) - skipped entirely in the fixed-step test mode.
        double worst_ratio = 0.0;
        if (!fixed) {
            bool ok = true;
            double shrink_factor = kInf;
            for (std::size_t i = 0; i < circuit.devices.size(); ++i) {
                if (!circuit.devices[i]->is_dynamic()) {
                    continue;
                }
                double ratio = 0.0;
                if (!circuit.devices[i]->lte_ok(
                        step, x, circuit.node_count(), settings.reltol, settings.chgtol,
                        settings.trtol, state.state_.data() + circuit.state_span[i].begin,
                        &ratio)) {
                    ok = false;
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
                if (shrink_factor < kInf) {
                    dt *= shrink_factor;
                }
                if (!std::isfinite(dt) || dt < dtmin) {
                    Diagnostic d = march_diag(
                        ErrorKind::Timestep, "spice.timestep.too_small",
                        "timestep fell below " + fmt_value(dtmin) + " s at t = " +
                            fmt_value(now) +
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
                circuit.devices[i]->commit(x, circuit.node_count(), step,
                                           state.state_.data() +
                                               circuit.state_span[i].begin);
            }
        }
        ++accepted;
        consecutive_rejects = 0;
        be_heuristic = false;
        be_next = lands_bp;  // §6.1: BE restart after a breakpoint landing

        if (!fixed) {
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

        // Progress: a false return stops the march (early abort on the
        // caller's own criterion - a diverging run, a UI closing).
        if (hooks.on_accept && !hooks.on_accept(now, tstop)) {
            stopped = true;
            return {};
        }
        return {};
    }
    return {};  // tstop reached: nothing to do
}

// --- TranMarch ---------------------------------------------------------------------

TranMarch::TranMarch(const Circuit& circuit, RunState& state, const RunSettings& settings,
                     const TranCard& card, const ParamTable& scope,
                     const std::vector<IcCard>& ics)
    : impl_(std::make_unique<Impl>(circuit, state, settings, card, scope, ics)) {}

TranMarch::~TranMarch() = default;

ws::core::Status TranMarch::prepare() {
    if (impl_->circuit.devices.empty()) {
        Diagnostic d = march_diag(ErrorKind::Semantic, "spice.semantic.empty_circuit",
                                  "no devices to simulate");
        d.analysis = ".tran";
        d.hint = "a deck needs device cards (R, V, I, ...) before its analyses";
        return spice_fail(std::move(d));
    }
    WS_TRY(impl_->validate_card());
    impl_->kernel = std::make_unique<DcKernel>(impl_->circuit, impl_->state,
                                               impl_->settings);
    if (const ws::core::Status st = impl_->kernel->prepare(); !st) {
        return wrap_solver_error(st, ".tran");
    }
    WS_TRY(impl_->seed_t0());
    impl_->prepared = true;
    return {};
}

ws::core::Status TranMarch::step(const Hooks& hooks) {
    return impl_->one_step(hooks);
}

bool TranMarch::done() const {
    return impl_->now >= impl_->tstop - 1e-12 * std::max(1.0, impl_->tstop);
}

bool TranMarch::stopped() const { return impl_->stopped; }

double TranMarch::now() const { return impl_->now; }

double TranMarch::tstop() const { return impl_->tstop; }

Table TranMarch::table() const { return impl_->table; }

Table TranMarch::take_table() { return std::move(impl_->table); }

AnalysisReport TranMarch::report() const {
    AnalysisReport r;
    r.analysis = ".tran";
    r.newton_iterations = impl_->total_iterations;
    r.lu_factorizations = impl_->kernel ? impl_->kernel->factorizations() : 0;
    r.stage = impl_->fixed ? "trap(fixed)" : "trap";
    r.accepted_steps = impl_->accepted;
    r.rejected_steps = impl_->rejected;
    return r;
}

const std::vector<double>& TranMarch::solution() const { return impl_->x; }

}  // namespace drivers

// --- TransientRun::Impl ---------------------------------------------------------------

class TransientRun::Impl {
public:
    Impl(drivers::TranMarch march) : march_(std::move(march)) {}

    drivers::TranMarch march_;
    StopToken stop_;
    std::function<void(double, double)> progress_;
    bool cancelled_ = false;
};

TransientRun::~TransientRun() = default;

ws::core::Status TransientRun::advance_to(double t) {
    const double target = std::clamp(t, impl_->march_.now(), impl_->march_.tstop());
    drivers::TranMarch::Hooks hooks;
    hooks.stop = &impl_->stop_;
    // Progress observes, never stops (the stop token and a false return
    // from the march-level hook are the stopping mechanisms).
    hooks.on_accept = [this](double now, double tstop) {
        if (impl_->progress_) {
            impl_->progress_(now, tstop);
        }
        return true;
    };
    while (!impl_->march_.done() && !impl_->march_.stopped() &&
           impl_->march_.now() < target - 1e-15 * std::max(1.0, target)) {
        // Cancellation check also at the loop head: a step lands exactly
        // on the target often, and the stop must not wait for the next
        // advance_to().
        if (impl_->stop_.stop_requested()) {
            impl_->cancelled_ = true;
            return {};
        }
        WS_TRY(impl_->march_.step(hooks));
    }
    if (impl_->march_.stopped()) {
        impl_->cancelled_ = true;
    }
    return {};
}

bool TransientRun::done() const { return impl_->march_.done(); }

bool TransientRun::cancelled() const { return impl_->cancelled_; }

double TransientRun::now() const { return impl_->march_.now(); }

double TransientRun::tstop() const { return impl_->march_.tstop(); }

const std::vector<double>& TransientRun::solution() const {
    return impl_->march_.solution();
}

Table TransientRun::table() const { return impl_->march_.table(); }

Table TransientRun::take_table() { return impl_->march_.take_table(); }

AnalysisReport TransientRun::report() const { return impl_->march_.report(); }

void TransientRun::set_stop_token(StopToken st) { impl_->stop_ = std::move(st); }

void TransientRun::set_progress(std::function<void(double, double)> cb) {
    impl_->progress_ = std::move(cb);
}

// The Simulator factory (needs the march over the Simulator's own state).
ws::core::Result<TransientRun> Simulator::begin_transient(
    const TranAnalysis& a, const std::vector<IcNode>& ics) {
    TranCard card;
    card.tstep = ExprProgram::constant(a.tstep > 0.0 ? a.tstep : a.tstop / 100.0);
    card.tstop = ExprProgram::constant(a.tstop);
    if (a.tstart) {
        card.tstart = ExprProgram::constant(*a.tstart);
        card.has_tstart = true;
    }
    if (a.tmax) {
        card.tmax = ExprProgram::constant(*a.tmax);
        card.has_tmax = true;
    }
    card.uic = a.uic;

    std::vector<IcCard> cards;
    if (!ics.empty()) {
        IcCard ic;
        ic.loc = SourceLoc{};
        for (const IcNode& n : ics) {
            Token tok;
            tok.text = n.node;
            ic.node_values.emplace_back(std::move(tok),
                                        ExprProgram::constant(n.volts));
        }
        cards.push_back(std::move(ic));
    }

    auto march = std::make_unique<drivers::TranMarch>(*circuit_, state_, settings_, card,
                                                      circuit_->params, cards);
    WS_TRY(march->prepare());
    TransientRun run;
    run.impl_ = std::make_unique<TransientRun::Impl>(std::move(*march));
    return run;
}

}  // namespace ws::spice
