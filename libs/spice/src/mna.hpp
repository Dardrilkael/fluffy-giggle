#pragma once

// Internal (src-only): MNA assembly over the ws::math sparse facade
// (DESIGN.md §2.2/§4 - the MNA policy layer, a wrapper over the solver,
// never a re-export). System shape: [G B; C D][v; i] = [z; e] with the
// ground row/col dropped - node ids ARE voltage-unknown indices and branch
// rows number after the nodes.
//
// M2: one pattern is the union of the DC, transient-companion and AC
// stamps (§3.2 - the dry run IS the pattern); the numeric phase runs
// exactly one of them per assembly pass. The complex twin (.ac) builds
// its matrix over the same frozen pattern.
//
// §3.5 rev: loads run over an IMMUTABLE circuit plus a per-run RunState;
// the system re-points each context's parameter/state slices between
// devices. Pattern mode points them at zeroed scratch sized to the
// circuit's largest device.

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <ws/math/sparse.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

// src-only detail namespace: the MNA pattern/numeric load contexts live
// here (defined in mna.cpp). A named detail namespace - NOT an anonymous
// one - so the friend declarations inside MnaSystem refer to the same
// types as the definitions. An anonymous namespace would put the
// definitions in ws::spice::<anon>::, a different type than the friend-
// declared ws::spice::detail:: names; MSVC then flags the unqualified
// reference as ambiguous, while GCC happens to resolve it. Spelling it
// out keeps both compilers happy.
namespace detail {
class PatternContext;
class NumericContext;
class AcPatternContext;
class AcNumericContext;
}  // namespace detail

class MnaSystem {
public:
    MnaSystem(std::size_t node_count, std::size_t branch_count,
              const Circuit& circuit);

    // The pattern dry run over every device (DC + tran + AC stamps - the
    // union, §3.2), then the symbolic analyze - once per circuit.
    [[nodiscard]] ws::core::Status setup(
        const std::vector<std::unique_ptr<Device>>& devices);

    // Builds the complex matrix + LU over the SAME frozen pattern (.ac).
    // Call after setup(); the real side is untouched.
    [[nodiscard]] ws::core::Status setup_ac();

    // One numeric assembly pass each: zero() + every device's load_dc /
    // load_tran / load_ac stamp, with this run's parameter and state
    // slices re-pointed per device. §3.6 (M3): `x` (when not null) is the
    // current iterate the nonlinear devices linearize against; `gmin` is
    // the run's junction-parallel conductance floor (§5.2 ladder stage 2)
    // folded into each nonlinear device's stamp. Pattern mode never calls
    // these - it runs through the contexts directly in setup().
    void load(const std::vector<std::unique_ptr<Device>>& devices,
              const RunState& rs, const std::vector<double>* x, double gmin);
    void load_tran(const std::vector<std::unique_ptr<Device>>& devices,
                   const RunState& rs, const TranStep& step,
                   const std::vector<double>* x, double gmin);
    void load_ac(const std::vector<std::unique_ptr<Device>>& devices,
                 const RunState& rs, double omega);

    // Freezes the pattern (col-major dedupe) and runs the symbolic
    // analyze. Fails through Status on a malformed pattern.
    [[nodiscard]] ws::core::Status analyze();

    // --- numeric phase ------------------------------------------------------
    void zero();  // values + rhs, pattern kept
    void zero_ac();

    // Factorizes over the current values and solves; x receives the
    // unknowns. Returns Singular/... errors as-is from the facade.
    [[nodiscard]] ws::core::Status solve(std::vector<double>& x);
    [[nodiscard]] ws::core::Status solve_ac(std::vector<std::complex<double>>& x);

    // §6.5: enforces v(node) = volts during an OP solve by stamping a
    // Norton (gforce from node to ground, gforce*volts injected) over the
    // already-loaded system. The transient itself never includes it.
    void force_node(std::size_t node, double volts, double gforce);

    // Largest |diagonal| over the currently loaded real values (the .ic
    // force scale, so gforce dominates the circuit by ~1e9 whatever its
    // impedance level).
    [[nodiscard]] double max_diagonal() const;

    [[nodiscard]] std::size_t dim() const { return node_count_ + branch_count_; }
    [[nodiscard]] std::size_t factorization_count() const {
        return lu_.factorization_count() + ac_lu_.factorization_count();
    }

    // Diagnostic counter (§9.4): assembly passes run.
    [[nodiscard]] std::size_t load_count() const { return load_count_; }

private:
    std::size_t node_count_;
    std::size_t branch_count_;
    const Circuit& circuit_;  // arena layout for the per-device slices
    ws::math::SparsePatternBuilder builder_;
    ws::math::SparsePattern pattern_;  // frozen by analyze()
    std::optional<ws::math::SparseMatrix<double>> matrix_;
    ws::math::SparseLu lu_;
    std::vector<double> rhs_;
    std::optional<ws::math::SparseMatrix<std::complex<double>>> ac_matrix_;
    ws::math::ComplexSparseLu ac_lu_;
    std::vector<std::complex<double>> ac_rhs_;
    std::size_t load_count_ = 0;
    // Zeroed scratch for the pattern dry run (sized to the largest
    // device's param/state slot count in the circuit).
    std::vector<double> scratch_params_;
    std::vector<double> scratch_state_;

    friend class detail::PatternContext;
    friend class detail::NumericContext;
    friend class detail::AcPatternContext;
    friend class detail::AcNumericContext;
};

}  // namespace ws::spice
