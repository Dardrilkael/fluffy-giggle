#include <ws/math/sparse.hpp>

#include <string>

// Eigen lives only in this TU - ws::math's private implementation detail,
// never in a header (workspace rule: a lib never leaks its dependencies).
// The eigen3 CMake target carries its include dir as SYSTEM, so its
// warnings stay out of our -Werror build; the MSVC/clang pragmas below are
// the same belt-and-braces as src/lu.cpp.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <Eigen/SparseLU>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ws::math {

namespace {

// Error codes for the sparse facade (category "math"). Small, stable, and
// only meaningful inside this file - callers branch on the Status, not the
// number (ws::core carries the message).
constexpr std::int32_t kErrNotAnalyzed = 1;
constexpr std::int32_t kErrNotFactored = 2;
constexpr std::int32_t kErrDimension = 3;
constexpr std::int32_t kErrPatternMismatch = 4;
constexpr std::int32_t kErrSingular = 5;
constexpr std::int32_t kErrBadPattern = 6;

[[nodiscard]] ws::core::unexpected<ws::core::Error> sparse_fail(
    std::int32_t code, std::string message) {
    return ws::core::fail(code, "math", std::move(message));
}

// The solver type: Eigen's left-looking sparse LU with partial pivoting,
// COLAMD fill-reducing ordering in analyzePattern (the facade contract's
// "ordering owned by the backend", DESIGN.md §4.2).
using RealSolver =
    Eigen::SparseLU<Eigen::SparseMatrix<double, Eigen::ColMajor>,
                    Eigen::COLAMDOrdering<int>>;
using ComplexSolver =
    Eigen::SparseLU<Eigen::SparseMatrix<std::complex<double>, Eigen::ColMajor>,
                    Eigen::COLAMDOrdering<int>>;

template <class Scalar>
Eigen::SparseMatrix<Scalar, Eigen::ColMajor> to_eigen(const SparsePattern& p,
                                                      const std::vector<Scalar>& values) {
    std::vector<Eigen::Triplet<Scalar>> triplets;
    triplets.reserve(p.nnz());
    for (std::size_t col = 0; col < p.n; ++col) {
        for (std::size_t k = p.col_starts[col]; k < p.col_starts[col + 1]; ++k) {
            triplets.emplace_back(static_cast<Eigen::Index>(p.row_indices[k]),
                                  static_cast<Eigen::Index>(col), values[k]);
        }
    }
    Eigen::SparseMatrix<Scalar, Eigen::ColMajor> m(static_cast<Eigen::Index>(p.n),
                                                   static_cast<Eigen::Index>(p.n));
    m.setFromTriplets(triplets.begin(), triplets.end());
    return m;
}

// A^T over the same values, for the adjoint solve (DESIGN.md §4.5).
template <class Scalar>
Eigen::SparseMatrix<Scalar, Eigen::ColMajor> to_eigen_transpose(
    const SparsePattern& p, const std::vector<Scalar>& values) {
    std::vector<Eigen::Triplet<Scalar>> triplets;
    triplets.reserve(p.nnz());
    for (std::size_t col = 0; col < p.n; ++col) {
        for (std::size_t k = p.col_starts[col]; k < p.col_starts[col + 1]; ++k) {
            triplets.emplace_back(static_cast<Eigen::Index>(col),
                                  static_cast<Eigen::Index>(p.row_indices[k]), values[k]);
        }
    }
    Eigen::SparseMatrix<Scalar, Eigen::ColMajor> m(static_cast<Eigen::Index>(p.n),
                                                   static_cast<Eigen::Index>(p.n));
    m.setFromTriplets(triplets.begin(), triplets.end());
    return m;
}

// Pattern sanity: n > 0, col_starts consistent, row indices < n and sorted
// per column (lower_bound in SparseMatrix::slot relies on the sort).
bool pattern_ok(const SparsePattern& p) {
    if (p.n == 0 || p.col_starts.size() != p.n + 1) {
        return false;
    }
    if (p.col_starts.front() != 0 || p.col_starts.back() != p.row_indices.size()) {
        return false;
    }
    for (std::size_t col = 0; col < p.n; ++col) {
        if (p.col_starts[col] > p.col_starts[col + 1]) {
            return false;
        }
        for (std::size_t k = p.col_starts[col] + 1; k < p.col_starts[col + 1]; ++k) {
            if (p.row_indices[k - 1] >= p.row_indices[k] ||
                p.row_indices[k] >= p.n) {
                return false;
            }
        }
        if (p.col_starts[col] < p.col_starts[col + 1] &&
            p.row_indices[p.col_starts[col]] >= p.n) {
            return false;
        }
    }
    return true;
}

bool same_pattern(const SparsePattern& a, const SparsePattern& b) {
    return a.n == b.n && a.col_starts == b.col_starts && a.row_indices == b.row_indices;
}

}  // namespace

// --- SparsePatternBuilder ----------------------------------------------------

SparsePattern SparsePatternBuilder::build(std::size_t n) {
    std::sort(entries_.begin(), entries_.end(),
              [](const auto& lhs, const auto& rhs) {
                  // column-major, then row.
                  return lhs.second != rhs.second ? lhs.second < rhs.second
                                                  : lhs.first < rhs.first;
              });
    SparsePattern p;
    p.n = n;
    p.col_starts.assign(n + 1, 0);
    p.row_indices.reserve(entries_.size());
    std::size_t last_col = static_cast<std::size_t>(-1);
    std::size_t last_row = static_cast<std::size_t>(-1);
    for (const auto& [row, col] : entries_) {
        if (row == last_row && col == last_col) {
            continue;  // duplicate stamp - legal, merged
        }
        p.row_indices.push_back(row);
        if (col != last_col) {
            for (std::size_t c = (last_col == static_cast<std::size_t>(-1) ? 0 : last_col + 1);
                 c <= col; ++c) {
                p.col_starts[c] = p.row_indices.size() - 1;
            }
        }
        last_row = row;
        last_col = col;
    }
    for (std::size_t c = (last_col == static_cast<std::size_t>(-1) ? 0 : last_col + 1);
         c <= n; ++c) {
        p.col_starts[c] = p.row_indices.size();
    }
    return p;
}

// --- SparseLu (real) ----------------------------------------------------------

struct SparseLu::Impl {
    SparsePattern pattern;
    RealSolver lu;
    bool analyzed = false;
    bool factored = false;
    std::size_t factorizations = 0;

    // Latest numeric values (copied at factorize) - what the lazy transpose
    // factors when a solve_transpose lands between factorizations.
    std::vector<double> values;

    // Lazy transpose factors, invalidated by every factorize().
    RealSolver lu_t;
    bool t_analyzed = false;
    bool t_factored = false;
};

SparseLu::SparseLu() : impl_(std::make_unique<Impl>()) {}
SparseLu::~SparseLu() = default;
SparseLu::SparseLu(SparseLu&&) noexcept = default;
SparseLu& SparseLu::operator=(SparseLu&&) noexcept = default;

ws::core::Status SparseLu::analyze(SparsePattern pattern) {
    if (!pattern_ok(pattern)) {
        return sparse_fail(kErrBadPattern, "sparse LU analyze: malformed pattern");
    }
    // Eigen's solver is non-assignable; a fresh Impl is the reset.
    impl_ = std::make_unique<Impl>();
    impl_->pattern = std::move(pattern);
    impl_->lu.analyzePattern(
        to_eigen<double>(impl_->pattern, std::vector<double>(impl_->pattern.nnz())));
    // No info() here: Eigen only initializes it at factorize time - a
    // symbolic problem surfaces as the first factorize's error instead.
    impl_->analyzed = true;
    impl_->factored = false;
    impl_->t_analyzed = false;
    impl_->t_factored = false;
    return {};
}

ws::core::Status SparseLu::factorize(const SparseMatrix<double>& a) {
    if (!impl_->analyzed) {
        return sparse_fail(kErrNotAnalyzed, "sparse LU factorize: analyze() first");
    }
    if (!same_pattern(a.pattern(), impl_->pattern)) {
        return sparse_fail(
            kErrPatternMismatch,
            "sparse LU factorize: matrix pattern differs from the analyzed pattern");
    }
    impl_->lu.factorize(to_eigen<double>(impl_->pattern, a.values()));
    if (impl_->lu.info() != Eigen::Success) {
        impl_->factored = false;
        impl_->t_factored = false;
        return sparse_fail(
            kErrSingular,
            "sparse LU factorize: matrix is singular or numerically broken (" +
                std::string(impl_->lu.lastErrorMessage()) + ")");
    }
    impl_->factored = true;
    impl_->t_factored = false;  // values moved under the transpose factors
    impl_->values = a.values();
    ++impl_->factorizations;
    return {};
}

ws::core::Status SparseLu::solve(std::span<const double> b, std::span<double> x) const {
    if (!impl_->factored) {
        return sparse_fail(kErrNotFactored, "sparse LU solve: factorize() first");
    }
    const std::size_t n = impl_->pattern.n;
    if (b.size() != n || x.size() != n) {
        return sparse_fail(kErrDimension, "sparse LU solve: dimension mismatch");
    }
    Eigen::Map<const Eigen::VectorXd> be(b.data(), static_cast<Eigen::Index>(n));
    Eigen::Map<Eigen::VectorXd> xe(x.data(), static_cast<Eigen::Index>(n));
    xe = impl_->lu.solve(be);
    if (!xe.allFinite()) {
        return sparse_fail(kErrSingular, "sparse LU solve: non-finite solution");
    }
    return {};
}

ws::core::Status SparseLu::solve_transpose(std::span<const double> b, std::span<double> x) {
    if (!impl_->factored) {
        return sparse_fail(kErrNotFactored, "sparse LU transpose solve: factorize() first");
    }
    const std::size_t n = impl_->pattern.n;
    if (b.size() != n || x.size() != n) {
        return sparse_fail(kErrDimension, "sparse LU transpose solve: dimension mismatch");
    }
    if (!impl_->t_analyzed) {
        // Symbolic for A^T, once per topology. (No info() check: Eigen only
        // initializes it at factorize time - same as analyze().)
        const auto zeros = std::vector<double>(impl_->pattern.nnz());
        impl_->lu_t.analyzePattern(to_eigen_transpose<double>(impl_->pattern, zeros));
        impl_->t_analyzed = true;
        impl_->t_factored = false;
    }
    if (!impl_->t_factored) {
        impl_->lu_t.factorize(
            to_eigen_transpose<double>(impl_->pattern, impl_->values));
        if (impl_->lu_t.info() != Eigen::Success) {
            return sparse_fail(kErrSingular, "sparse LU transpose factorize: singular");
        }
        impl_->t_factored = true;
    }
    Eigen::Map<const Eigen::VectorXd> be(b.data(), static_cast<Eigen::Index>(n));
    Eigen::Map<Eigen::VectorXd> xe(x.data(), static_cast<Eigen::Index>(n));
    xe = impl_->lu_t.solve(be);
    if (!xe.allFinite()) {
        return sparse_fail(kErrSingular, "sparse LU transpose solve: non-finite solution");
    }
    return {};
}

bool SparseLu::analyzed() const { return impl_->analyzed; }
bool SparseLu::factored() const { return impl_->factored; }
std::size_t SparseLu::factorization_count() const { return impl_->factorizations; }

// --- ComplexSparseLu ------------------------------------------------------------

struct ComplexSparseLu::Impl {
    SparsePattern pattern;
    ComplexSolver lu;
    bool analyzed = false;
    bool factored = false;
    std::size_t factorizations = 0;
};

ComplexSparseLu::ComplexSparseLu() : impl_(std::make_unique<Impl>()) {}
ComplexSparseLu::~ComplexSparseLu() = default;
ComplexSparseLu::ComplexSparseLu(ComplexSparseLu&&) noexcept = default;
ComplexSparseLu& ComplexSparseLu::operator=(ComplexSparseLu&&) noexcept = default;

ws::core::Status ComplexSparseLu::analyze(SparsePattern pattern) {
    if (!pattern_ok(pattern)) {
        return sparse_fail(kErrBadPattern, "complex sparse LU analyze: malformed pattern");
    }
    impl_ = std::make_unique<Impl>();
    impl_->pattern = std::move(pattern);
    impl_->lu.analyzePattern(to_eigen<std::complex<double>>(
        impl_->pattern, std::vector<std::complex<double>>(impl_->pattern.nnz())));
    impl_->analyzed = true;
    impl_->factored = false;
    return {};
}

ws::core::Status ComplexSparseLu::factorize(
    const SparseMatrix<std::complex<double>>& a) {
    if (!impl_->analyzed) {
        return sparse_fail(kErrNotAnalyzed, "complex sparse LU factorize: analyze() first");
    }
    if (!same_pattern(a.pattern(), impl_->pattern)) {
        return sparse_fail(
            kErrPatternMismatch,
            "complex sparse LU factorize: matrix pattern differs from the analyzed pattern");
    }
    impl_->lu.factorize(
        to_eigen<std::complex<double>>(impl_->pattern, a.values()));
    if (impl_->lu.info() != Eigen::Success) {
        impl_->factored = false;
        return sparse_fail(
            kErrSingular,
            "complex sparse LU factorize: matrix is singular or numerically broken (" +
                std::string(impl_->lu.lastErrorMessage()) + ")");
    }
    impl_->factored = true;
    ++impl_->factorizations;
    return {};
}

ws::core::Status ComplexSparseLu::solve(std::span<const std::complex<double>> b,
                                        std::span<std::complex<double>> x) const {
    if (!impl_->factored) {
        return sparse_fail(kErrNotFactored, "complex sparse LU solve: factorize() first");
    }
    const std::size_t n = impl_->pattern.n;
    if (b.size() != n || x.size() != n) {
        return sparse_fail(kErrDimension, "complex sparse LU solve: dimension mismatch");
    }
    Eigen::Map<const Eigen::VectorXcd> be(b.data(), static_cast<Eigen::Index>(n));
    Eigen::Map<Eigen::VectorXcd> xe(x.data(), static_cast<Eigen::Index>(n));
    xe = impl_->lu.solve(be);
    if (!xe.allFinite()) {
        return sparse_fail(kErrSingular, "complex sparse LU solve: non-finite solution");
    }
    return {};
}

bool ComplexSparseLu::analyzed() const { return impl_->analyzed; }
bool ComplexSparseLu::factored() const { return impl_->factored; }
std::size_t ComplexSparseLu::factorization_count() const { return impl_->factorizations; }

}  // namespace ws::math
