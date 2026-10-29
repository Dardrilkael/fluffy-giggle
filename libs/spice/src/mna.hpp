#pragma once

// Internal (src-only): MNA assembly over the ws::math sparse facade
// (DESIGN.md §2.2/§4 - the MNA policy layer, a wrapper over the solver,
// never a re-export). System shape: [G B; C D][v; i] = [z; e] with the
// ground row/col dropped - node ids ARE voltage-unknown indices and branch
// rows number after the nodes.

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <ws/math/sparse.hpp>
#include <ws/spice/device.hpp>

namespace ws::spice {

class MnaSystem {
public:
    MnaSystem(std::size_t node_count, std::size_t branch_count);

    // The pattern dry run over every device, then the symbolic analyze
    // (§3.2) - once per circuit. The context classes are private to
    // mna.cpp; devices load through these two passes only.
    [[nodiscard]] ws::core::Status setup(
        const std::vector<std::unique_ptr<Device>>& devices);

    // One numeric assembly pass: zero() + every device's load_dc.
    void load(const std::vector<std::unique_ptr<Device>>& devices);

    // Freezes the pattern (col-major dedupe) and runs the symbolic
    // analyze. Fails through Status on a malformed pattern.
    [[nodiscard]] ws::core::Status analyze();

    // --- numeric phase ------------------------------------------------------
    void zero();  // values + rhs, pattern kept

    // Factorizes over the current values and solves; x receives the
    // unknowns. Returns Singular/... errors as-is from the facade.
    [[nodiscard]] ws::core::Status solve(std::vector<double>& x);

    [[nodiscard]] std::size_t dim() const { return node_count_ + branch_count_; }
    [[nodiscard]] std::size_t factorization_count() const {
        return lu_.factorization_count();
    }

    // Diagnostic counter (§9.4): assembly passes run.
    [[nodiscard]] std::size_t load_count() const { return load_count_; }

private:
    std::size_t node_count_;
    std::size_t branch_count_;
    ws::math::SparsePatternBuilder builder_;
    std::optional<ws::math::SparseMatrix<double>> matrix_;
    ws::math::SparseLu lu_;
    std::vector<double> rhs_;
    std::size_t load_count_ = 0;

    friend class PatternContext;
    friend class NumericContext;
};

}  // namespace ws::spice
