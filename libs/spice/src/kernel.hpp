#pragma once

// Internal (src-only): the shared DC/OP Newton kernel and save-list
// helpers behind every driver (op/dc/tran/ac) and the steppable
// TransientRun. One kernel, one state model - the CLI path and the
// C++ path cannot drift apart.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/spice/circuit.hpp>
#include "mna.hpp"

#include <ws/spice/deck.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice::detail {

inline Diagnostic run_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

// --- the default save-list (§0.5) ----------------------------------------------

struct SaveList {
    std::vector<std::string> columns;
    std::vector<std::size_t> nodes;      // node ids (v(name) columns)
    std::vector<std::size_t> branches;   // branch ids (i(name) columns)
};

inline SaveList default_save_list(const Circuit& c) {
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
inline void append_row_columns(const Circuit& c, const SaveList& s, Table& t,
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
inline std::vector<Signal> signal_skeleton(const SaveList& s) {
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
inline void fold_all(const Circuit& c, RunState& rs) {
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
    //
    // §5.2 (rev: PTC promoted from the backlog): the FIXED ladder -
    //   1. plain Newton,
    //   2. gmin homotopy (junction-parallel conductance down a geometric
    //      staircase to the deck's gmin; every stage warm-started),
    //   3. source homotopy (every independent source scaled 0 -> 1),
    //   4. pseudo-transient continuation (a shunt capacitor per node,
    //      integrated in pseudo-time with a growing step until the
    //      derivatives vanish - what rescues oscillators and latches
    //      that stall stages 1-3).
    // The stage that converged lands in `stage_` (the §9.4 record);
    // every stage's final pass is an un-aided plain Newton confirm.
    ws::core::Status solve(std::vector<double>& x, int max_iterations,
                           const std::string& context, std::size_t& iterations,
                           const std::vector<std::pair<std::size_t, double>>* forces =
                               nullptr) {
        stage_ = "newton";
        if (newton_pass(x, max_iterations, context, iterations, forces)) {
            return {};
        }
        // Stage 2: gmin staircase (§5.2). g_k = gmin * 10^k, k = steps..0;
        // each solve warm-starts from the previous, and the k=0 pass IS
        // the plain endpoint.
        stage_ = "gmin";
        for (int k = settings_.gmin_steps; k >= 0; --k) {
            const double g = settings_.gmin * std::pow(10.0, static_cast<double>(k));
            if (!newton_pass(x, max_iterations, context, iterations, forces, g)) {
                break;
            }
            if (k == 0) {
                return {};
            }
        }
        // Stage 3: source homotopy (§5.2). The independent sources' dc
        // parameter slots scale 0 -> 1 over srcsteps warm-started solves;
        // values only (the pattern cannot move, §3.5 rev), restored by
        // RAII whatever happens.
        stage_ = "source";
        {
            SourceScale scale(circuit_, state_);
            for (int k = 1; k <= settings_.src_steps; ++k) {
                scale.apply(static_cast<double>(k) /
                            static_cast<double>(settings_.src_steps));
                if (!newton_pass(x, max_iterations, context, iterations, forces)) {
                    break;
                }
                if (k == settings_.src_steps) {
                    return {};
                }
            }
        }
        // Stage 4: pseudo-transient continuation (§5.2 rev, promoted from
        // the backlog). Every node gets a shunt conductance c (a pseudo
        // capacitor C = c*dt integrated in BE pseudo-time): the system
        // (G + cI) x = b + c x_prev is solved repeatedly while c decays
        // (the pseudo timestep grows), warm-started; switch states ride
        // the pseudo-dynamics, so latches and oscillators settle where
        // the homotopies chatter. A final plain Newton confirms.
        stage_ = "ptc";
        if (ptc_pass(x, max_iterations, context, iterations, forces)) {
            return {};
        }
        // The ladder failed: report the richest context we have.
        stage_ = "failed";
        Diagnostic d = run_diag(
            ErrorKind::Convergence, "spice.convergence.ladder",
            "no convergence after newton, gmin stepping, source stepping and"
            " pseudo-transient continuation");
        d.analysis = context;
        return spice_fail(std::move(d));
    }

    [[nodiscard]] const std::string& stage() const { return stage_; }

    // The plain §5.1 Newton loop. `shunt_g` (when > 0) adds a conductance
    // from every NODE to ground - the gmin stage; `shunt_ref` (when not
    // null) makes it a Norton towards those values instead - the PTC
    // pseudo-companion. Returns ok/nok without building a Diagnostic (the
    // ladder builds the final one with the full stage record).
    ws::core::Status newton_pass(
        std::vector<double>& x, int max_iterations, const std::string& context,
        std::size_t& iterations, const std::vector<std::pair<std::size_t, double>>* forces,
        double shunt_g = 0.0, const std::vector<double>* shunt_ref = nullptr) {
        (void)context;  // diagnostics are the ladder caller's job
        const std::size_t nc = circuit_.node_count();
        double gforce = 0.0;
        if (forces != nullptr && !forces->empty()) {
            // Measure-only load: nullptr for x means nonlinear devices
            // linearize around zero - fine for sizing gforce, and the
            // diode's Gd at Vd=0 is finite (Is/(n*Vt) + gmin).
            mna_.load(circuit_.devices, state_, nullptr, settings_.gmin);
            gforce = 1e9 * std::max(mna_.max_diagonal(), 1e-9);
        }
        if (x.size() != mna_.dim()) {
            x.assign(mna_.dim(), 0.0);
        }
        for (int it = 1; it <= max_iterations; ++it) {
            ++iterations;
            // §3.6: thread the current iterate `x` so nonlinear devices
            // (the diode) linearize against it. `x` here is x[k-1] - the
            // previous iterate / warm start.
            mna_.load(circuit_.devices, state_, &x, settings_.gmin);
            // §5.3 (M3): opt-in gshunt - a per-node conductance to ground
            // that masks floating-node stiffness. Default 0 (off); a
            // positive value shunts every node diagonal in every Newton
            // load (the §5.2 ladder's steady aid, always-on when the
            // deck asks for it).
            if (settings_.gshunt > 0.0) {
                for (std::size_t n = 0; n < nc; ++n) {
                    mna_.force_node(n, 0.0, settings_.gshunt);
                }
            }
            if (forces != nullptr) {
                for (const auto& [node, volts] : *forces) {
                    mna_.force_node(node, volts, gforce);
                }
            }
            if (shunt_g > 0.0) {
                for (std::size_t n = 0; n < nc; ++n) {
                    const double target = (shunt_ref != nullptr) ? (*shunt_ref)[n] : 0.0;
                    mna_.force_node(n, target, shunt_g);
                }
            }
            std::vector<double> x_new;
            if (!mna_.solve(x_new)) {
                return ws::core::unexpected(ws::core::make_error(
                    static_cast<std::int32_t>(1), "math", "factorization failed"));
            }
            for (const double v : x_new) {
                if (!std::isfinite(v)) {
                    return ws::core::unexpected(ws::core::make_error(
                        static_cast<std::int32_t>(ErrorKind::Convergence), "spice",
                        "Newton iterate is not finite"));
                }
            }
            bool state_changed = false;
            for (std::size_t i = 0; i < circuit_.devices.size(); ++i) {
                // §3.6: pass &x as x_prev so nonlinear devices can advance
                // their pnjlim/reference slots (x is still the pre-solve
                // iterate here - it's moved into x_new BELOW).
                state_changed = circuit_.devices[i]->update_state(
                                    x_new, nc, state_.state_.data() +
                                                   circuit_.state_span[i].begin,
                                    &x) ||
                                state_changed;
            }
            bool converged = !state_changed;
            // §5.1 (M3): the nonlinear-residual check complements the
            // voltage-delta check. Both must pass. Linear devices return
            // {0,0} and never gate convergence.
            if (converged) {
                for (std::size_t i = 0; i < circuit_.devices.size(); ++i) {
                    const NonlinResid r = circuit_.devices[i]->nonlinear_residual(
                        state_.state_.data() + circuit_.state_span[i].begin);
                    const double tol = settings_.reltol * r.ref + settings_.abstol;
                    if (r.delta > tol) {
                        converged = false;
                        break;
                    }
                }
            }
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
        return ws::core::unexpected(ws::core::make_error(
            static_cast<std::int32_t>(ErrorKind::Convergence), "spice",
            "no convergence after " + std::to_string(max_iterations) +
                " Newton iterations"));
    }

    // §5.2 stage 3 helper: scales every V/I source's dc parameter slot,
    // restoring the originals on destruction (values only - the pattern
    // cannot move, §3.5 rev).
    class SourceScale {
    public:
        SourceScale(const Circuit& c, RunState& rs) : c_(c), rs_(rs) {
            for (std::size_t i = 0; i < c_.devices.size(); ++i) {
                if ((c_.devices[i]->kind() == 'V' || c_.devices[i]->kind() == 'I') &&
                    c_.param_span[i].count > 0) {
                    slots_.push_back(i);
                    originals_.push_back(rs_.params_[c_.param_span[i].begin]);
                }
            }
        }
        ~SourceScale() {
            for (std::size_t k = 0; k < slots_.size(); ++k) {
                rs_.params_[c_.param_span[slots_[k]].begin] = originals_[k];
            }
        }
        SourceScale(const SourceScale&) = delete;
        SourceScale& operator=(const SourceScale&) = delete;
        void apply(double scale) {
            for (std::size_t k = 0; k < slots_.size(); ++k) {
                rs_.params_[c_.param_span[slots_[k]].begin] = originals_[k] * scale;
            }
        }

    private:
        const Circuit& c_;
        RunState& rs_;
        std::vector<std::size_t> slots_;
        std::vector<double> originals_;
    };

    // §5.2 stage 4 (rev): pseudo-transient continuation, promoted from the
    // backlog. Every node carries a shunt pseudo-capacitor; the BE
    // companion (G + cI)x = b + c x_prev integrates pseudo-time while c
    // decays geometrically (the pseudo-step grows). Warm-started
    // throughout; switch states ride the pseudo-dynamics - that is what
    // settles chattering latches where the homotopies cannot. A final
    // un-aided Newton confirms the endpoint.
    bool ptc_pass(std::vector<double>& x, int max_iterations,
                  const std::string& context, std::size_t& iterations,
                  const std::vector<std::pair<std::size_t, double>>* forces) {
        // Measure-only load for the initial pseudo-capacitor sizing;
        // nullptr for x linearizes nonlinear devices around zero (fine
        // for max_diagonal, which only sizes the pseudo-C).
        mna_.load(circuit_.devices, state_, nullptr, settings_.gmin);
        double c = 1e3 * std::max(mna_.max_diagonal(), 1e-9);
        if (x.size() != mna_.dim()) {
            x.assign(mna_.dim(), 0.0);
        }
        for (int it = 0; it < max_iterations; ++it) {
            std::vector<double> x_prev = x;
            if (!newton_pass(x, 20, context, iterations, forces, c, &x_prev)) {
                return false;
            }
            c *= 0.5;
            if (c <= settings_.gmin) {
                break;
            }
        }
        return static_cast<bool>(newton_pass(x, max_iterations, context, iterations, forces));
    }

    // One transient step: companion load (coefficients from the devices'
    // begin_step over their state slices) -> Newton loop -> switch
    // re-evaluation. M3 (diode): this is now a real Newton loop (was
    // switch-only when every device was linear) - the same §5.1
    // voltage-delta + nonlinear-residual convergence gate as `newton_pass`,
    // just over `load_tran` and capped at itl4. Switch state changes still
    // force another solve (a switch flipping changes the circuit).
    ws::core::Status solve_tran(std::vector<double>& x, const TranStep& step,
                                const std::string& context, std::size_t& iterations) {
        const std::size_t nc = circuit_.node_count();
        for (int it = 1; it <= settings_.itl4; ++it) {
            ++iterations;
            // §3.6: thread the current iterate so the diode linearizes
            // against it (the tran companion from C/L is already in `x`
            // via begin_step's state slots).
            mna_.load_tran(circuit_.devices, state_, step, &x, settings_.gmin);
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
                                                   circuit_.state_span[i].begin,
                                    &x) ||
                                state_changed;
            }
            // §5.1 (M3): both the switch-state gate (a flip demands
            // another solve regardless of convergence) and the
            // voltage/residual gate. A switch flip forces another iteration;
            // otherwise the standard Newton convergence check decides.
            if (!state_changed) {
                bool converged = true;
                for (std::size_t i = 0; i < circuit_.devices.size(); ++i) {
                    const NonlinResid r = circuit_.devices[i]->nonlinear_residual(
                        state_.state_.data() + circuit_.state_span[i].begin);
                    const double tol = settings_.reltol * r.ref + settings_.abstol;
                    if (r.delta > tol) {
                        converged = false;
                        break;
                    }
                }
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
            } else {
                x = std::move(x_new);
            }
        }
        Diagnostic d = run_diag(
            ErrorKind::Convergence, "spice.convergence.tran",
            "transient step did not converge after " + std::to_string(settings_.itl4) +
                " Newton iterations (switch chatter or nonlinear stiffness?)");
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
    std::string stage_;  // §5.2: the ladder stage that converged (§9.4)
    const Circuit& circuit_;
    RunState& state_;
    const RunSettings& settings_;
    MnaSystem mna_;
};

}  // namespace ws::spice::detail
