#pragma once

#include <algorithm>
#include <cstddef>
#include <complex>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ws/core/assert.hpp>
#include <ws/core/result.hpp>

// Sparse LU with symbolic reuse - the numerics home for ws::spice's MNA
// solver (DESIGN.md §4). The facade is deliberately KLU-shaped: `analyze`
// once per topology (symbolic: pattern + fill-reducing ordering), then
// `factorize` per Newton/time step over the same pattern (numeric), `solve`
// against the stored factors, plus a transpose solve for the noise adjoint
// system. The backend (Eigen SparseLU today, SuiteSparse KLU once M0b
// vendors it) is PRIVATE: no Eigen type or header appears here, and the
// swap touches src/sparse.cpp only.
//
// Error model follows the house rule: ws::core::Status/Result, no
// exceptions across the boundary. Singularity is reported as an error
// carrying Eigen's last error message when there is one; naming the
// offending node is the caller's (elaboration-time graph) job.

namespace ws::math {

// CSC symbolic structure of a square sparse matrix. Immutable once built;
// row indices are sorted within each column and deduplicated.
struct SparsePattern {
    std::size_t n = 0;                        // matrix dimension
    std::vector<std::size_t> col_starts;      // n + 1 entries
    std::vector<std::size_t> row_indices;     // nnz entries, sorted per column

    std::size_t nnz() const { return row_indices.size(); }
};

// Collects (row, col) stamps and freezes them into a SparsePattern. This is
// what ws::spice's pattern-mode dry run records through (DESIGN.md §3.2:
// the load path IS the pattern); duplicate stamps are legal and merge.
class SparsePatternBuilder {
public:
    void reserve(std::size_t entries) { entries_.reserve(entries); }

    // row == col == dimensions are the caller's business; the builder only
    // stamps coordinates (ws::spice maps ground to a sentinel and skips it
    // before ever reaching here).
    void add(std::size_t row, std::size_t col) { entries_.emplace_back(row, col); }

    std::size_t size() const { return entries_.size(); }

    // Sorts by (col, row), deduplicates, returns the frozen pattern.
    SparsePattern build(std::size_t n);

private:
    std::vector<std::pair<std::size_t, std::size_t>> entries_;  // (row, col)
};

// Numeric values over a frozen pattern. The pattern is shared (many
// matrices over one topology), the values are per-factorization; `zero()`
// between assembly passes keeps the allocation.
template <class T>
class SparseMatrix {
public:
    SparseMatrix() = default;

    explicit SparseMatrix(SparsePattern pattern)
        : pattern_(std::move(pattern)), values_(pattern_.nnz(), T{}) {}

    const SparsePattern& pattern() const { return pattern_; }
    std::size_t dim() const { return pattern_.n; }
    std::size_t nnz() const { return pattern_.nnz(); }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Slot index into values() for an in-pattern coordinate, npos otherwise.
    // O(log nnz); callers stamping repeatedly (every Newton iteration) cache
    // the slot once at setup instead of re-searching per load.
    std::size_t slot(std::size_t row, std::size_t col) const {
        const std::size_t begin = pattern_.col_starts[col];
        const std::size_t end = pattern_.col_starts[col + 1];
        const auto* first = pattern_.row_indices.data() + begin;
        const auto* last = pattern_.row_indices.data() + end;
        const auto* it = std::lower_bound(first, last, row);
        if (it == last || *it != row) {
            return npos;
        }
        return begin + static_cast<std::size_t>(it - first);
    }

    bool contains(std::size_t row, std::size_t col) const {
        return slot(row, col) != npos;
    }

    // In-pattern access. A miss is a programmer error (a numeric-mode write
    // outside the recorded pattern - DESIGN.md §3.2 trips WS_CORE_ASSERT),
    // never a runtime failure.
    T& at(std::size_t row, std::size_t col) {
        const std::size_t s = slot(row, col);
        WS_CORE_ASSERT(s != npos);
        return values_[s];
    }

    // at(row, col) += v, with the same in-pattern assert.
    void add(std::size_t row, std::size_t col, T v) {
        const std::size_t s = slot(row, col);
        WS_CORE_ASSERT(s != npos);
        values_[s] += v;
    }

    const T& value_at_slot(std::size_t s) const { return values_[s]; }
    T& value_at_slot(std::size_t s) { return values_[s]; }

    const std::vector<T>& values() const { return values_; }
    std::vector<T>& values() { return values_; }

    // Values to zero, pattern kept - the per-assembly-pass reset.
    void zero() { std::fill(values_.begin(), values_.end(), T{}); }

private:
    SparsePattern pattern_;
    std::vector<T> values_;
};

// Real-valued sparse LU (double) with symbolic reuse. Move-only; cheap to
// hold by value, one per analysis in ws::spice's Simulator.
class SparseLu {
public:
    SparseLu();
    ~SparseLu();
    SparseLu(SparseLu&&) noexcept;
    SparseLu& operator=(SparseLu&&) noexcept;
    SparseLu(const SparseLu&) = delete;
    SparseLu& operator=(const SparseLu&) = delete;

    // Symbolic pass, once per topology: fill-reducing ordering over the
    // pattern. Rejects an empty pattern or a pattern whose row indices
    // exceed n (garbage coordinates are a caller bug; they surface here).
    ws::core::Status analyze(SparsePattern pattern);

    // Numeric factorization over the pattern's values. Repeated calls
    // (every Newton/time step) reuse the symbolic analysis; the values may
    // change, the pattern may not - a mismatched pattern is rejected.
    // Singularity (exact zero pivot) is an error, not an exception.
    ws::core::Status factorize(const SparseMatrix<double>& a);

    // x = A^-1 b against the stored factors; spans keep the hot path
    // allocation-free.
    ws::core::Status solve(std::span<const double> b, std::span<double> x) const;

    // Solves A^T x = b (the noise-adjoint system). The transpose
    // factorization is built lazily on first use and invalidated by every
    // factorize() - DESIGN.md §4.5: explicit per-frequency refactors, simple
    // and backend-agnostic.
    ws::core::Status solve_transpose(std::span<const double> b, std::span<double> x);

    bool analyzed() const;
    bool factored() const;

    // Diagnostics (DESIGN.md §9.4: LU refactors feed the run report).
    std::size_t factorization_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The complex twin over the same facade, for .ac systems.
class ComplexSparseLu {
public:
    ComplexSparseLu();
    ~ComplexSparseLu();
    ComplexSparseLu(ComplexSparseLu&&) noexcept;
    ComplexSparseLu& operator=(ComplexSparseLu&&) noexcept;
    ComplexSparseLu(const ComplexSparseLu&) = delete;
    ComplexSparseLu& operator=(const ComplexSparseLu&) = delete;

    ws::core::Status analyze(SparsePattern pattern);
    ws::core::Status factorize(const SparseMatrix<std::complex<double>>& a);
    ws::core::Status solve(std::span<const std::complex<double>> b,
                           std::span<std::complex<double>> x) const;

    bool analyzed() const;
    bool factored() const;
    std::size_t factorization_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::math
