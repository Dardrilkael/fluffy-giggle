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

}  // namespace ws::spice::detail
