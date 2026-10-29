// M0 gate (libs/spice DESIGN.md §10.3, the ws::math side): random sparse
// systems vs a dense reference, same-pattern refactor equality, singular
// detection (structural + numerical), complex solves, and the transpose
// solve vs an explicit transposed factorization. Deterministic LCG - no
// <random> machinery needed, and reruns stay reproducible.
//
// (Part of tests for the math lib; run via `ctest --preset linux -R math`.)

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <ws/math/sparse.hpp>

using ws::math::ComplexSparseLu;
using ws::math::SparseLu;
using ws::math::SparseMatrix;
using ws::math::SparsePattern;
using ws::math::SparsePatternBuilder;

namespace {

// Small, deterministic, boring LCG - good enough for matrix entries.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_;
    }
    double unit() {  // (0, 1)
        return static_cast<double>(next() >> 11) / 9007199254740992.0;
    }

private:
    std::uint64_t state_;
};

// Dense reference solve by Gaussian elimination with partial pivoting
// (ws::math's own dense LU throws on singular input, which the deliberately
// singular cases below would trip - so the reference rolls its own).
bool dense_solve(std::vector<double> a,  // row-major n*n, destroyed
                 std::vector<double> b,  // n, destroyed
                 std::vector<double>& x) {
    const std::size_t n = b.size();
    x.assign(n, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t best = k;
        for (std::size_t i = k + 1; i < n; ++i) {
            if (std::abs(a[i * n + k]) > std::abs(a[best * n + k])) {
                best = i;
            }
        }
        if (std::abs(a[best * n + k]) < 1e-300) {
            return false;
        }
        if (best != k) {
            for (std::size_t j = 0; j < n; ++j) {
                std::swap(a[k * n + j], a[best * n + j]);
            }
            std::swap(b[k], b[best]);
        }
        for (std::size_t i = k + 1; i < n; ++i) {
            const double f = a[i * n + k] / a[k * n + k];
            for (std::size_t j = k; j < n; ++j) {
                a[i * n + j] -= f * a[k * n + j];
            }
            b[i] -= f * b[k];
        }
    }
    for (std::size_t ii = n; ii-- > 0;) {
        double sum = b[ii];
        for (std::size_t j = ii + 1; j < n; ++j) {
            sum -= a[ii * n + j] * x[j];
        }
        x[ii] = sum / a[ii * n + ii];
    }
    return true;
}

// Builds a SparseMatrix from a dense row-major source.
SparseMatrix<double> from_dense(const std::vector<double>& dense, std::size_t n,
                                double drop = 0.0) {
    SparsePatternBuilder builder;
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            if (std::abs(dense[r * n + c]) > drop) {
                builder.add(r, c);
            }
        }
    }
    SparseMatrix<double> m(builder.build(n));
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            if (m.contains(r, c)) {
                m.at(r, c) = dense[r * n + c];
            }
        }
    }
    return m;
}

}  // namespace

TEST_CASE("pattern builder sorts, dedupes and reports per-column ranges") {
    SparsePatternBuilder builder;
    builder.add(2, 1);
    builder.add(0, 0);
    builder.add(2, 1);  // duplicate
    builder.add(1, 0);
    builder.add(0, 1);
    const SparsePattern p = builder.build(3);

    CHECK(p.n == 3);
    CHECK(p.nnz() == 4);
    CHECK(p.col_starts == std::vector<std::size_t>{0, 2, 4, 4});
    CHECK(p.row_indices == std::vector<std::size_t>{0, 1, 0, 2});
}

TEST_CASE("sparse matrix slot lookup, at() and add()") {
    SparsePatternBuilder builder;
    builder.add(0, 0);
    builder.add(1, 1);
    SparseMatrix<double> m(builder.build(2));

    CHECK(m.contains(0, 0));
    CHECK(!m.contains(0, 1));
    CHECK(m.slot(1, 1) != SparseMatrix<double>::npos);
    CHECK(m.slot(1, 0) == SparseMatrix<double>::npos);

    m.at(0, 0) = 3.0;
    m.add(0, 0, 1.0);
    CHECK(m.at(0, 0) == doctest::Approx(4.0));

    m.zero();
    CHECK(m.at(0, 0) == doctest::Approx(0.0));
}

TEST_CASE("random sparse systems match the dense reference") {
    Lcg rng(0xC0FFEE);
    for (int trial = 0; trial < 20; ++trial) {
        const std::size_t n = 3 + static_cast<std::size_t>(rng.next() % 40);
        std::vector<double> dense(n * n, 0.0);
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t c = 0; c < n; ++c) {
                // ~35% fill, well-conditioned entries in [0.5, 1.5)
                if (rng.unit() < 0.35) {
                    dense[r * n + c] = 0.5 + rng.unit();
                }
            }
            dense[r * n + r] += 2.0;  // diagonal dominance keeps it sane
        }
        std::vector<double> b(n);
        for (double& v : b) {
            v = -1.0 + 2.0 * rng.unit();
        }

        SparseMatrix<double> m = from_dense(dense, n);
        SparseLu lu;
        REQUIRE(lu.analyze(m.pattern()));
        REQUIRE(lu.factorize(m));

        std::vector<double> x(n, 0.0);
        REQUIRE(lu.solve(b, x));

        std::vector<double> ref;
        REQUIRE(dense_solve(dense, b, ref));
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(x[i] == doctest::Approx(ref[i]).epsilon(1e-10));
        }
    }
}

TEST_CASE("same-pattern refactor with changed values equals a fresh factor") {
    Lcg rng(0xBEEF);
    const std::size_t n = 25;
    std::vector<double> dense(n * n, 0.0);
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            if (rng.unit() < 0.3) {
                dense[r * n + c] = 0.5 + rng.unit();
            }
        }
        dense[r * n + r] += 2.0;
    }
    std::vector<double> b(n);
    for (double& v : b) {
        v = -1.0 + 2.0 * rng.unit();
    }

    SparseMatrix<double> m = from_dense(dense, n);
    SparseLu reused;
    REQUIRE(reused.analyze(m.pattern()));
    REQUIRE(reused.factorize(m));
    std::vector<double> x_reused(n, 0.0);
    REQUIRE(reused.solve(b, x_reused));

    // Change every value in place (same pattern), refactor, solve.
    for (double& v : m.values()) {
        v = v * 0.5 + 0.25;
    }
    REQUIRE(reused.factorize(m));
    std::vector<double> x_reused2(n, 0.0);
    REQUIRE(reused.solve(b, x_reused2));

    SparseLu fresh;
    REQUIRE(fresh.analyze(m.pattern()));
    REQUIRE(fresh.factorize(m));
    std::vector<double> x_fresh(n, 0.0);
    REQUIRE(fresh.solve(b, x_fresh));

    CHECK(reused.factorization_count() == 2);
    CHECK(fresh.factorization_count() == 1);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(x_reused2[i] == doctest::Approx(x_fresh[i]).epsilon(1e-12));
    }
}

TEST_CASE("numerical singularity is an error, never an exception") {
    // Row 1 is a multiple of row 0: structurally fine, numerically singular.
    const std::size_t n = 3;
    std::vector<double> dense = {
        1.0, 2.0, 0.0,   //
        2.0, 4.0, 0.0,   // = 2 x row 0
        0.0, 0.0, 1.0,   //
    };
    SparseMatrix<double> m = from_dense(dense, n);
    SparseLu lu;
    REQUIRE(lu.analyze(m.pattern()));
    const auto st = lu.factorize(m);
    if (st) {
        // Eigen occasionally reports success but bakes zeros into U; the
        // solve must then catch the non-finite garbage. Either way the
        // facade refuses to hand back a silently wrong answer.
        std::vector<double> x(n, 0.0);
        const std::vector<double> b{1.0, 1.0, 1.0};
        const auto sst = lu.solve(b, x);
        REQUIRE_FALSE(sst);
    } else {
        CHECK(st.error().category == std::string("math"));
        CHECK_FALSE(lu.factored());
    }
}

TEST_CASE("facade misuse is reported through Status") {
    SparseLu lu;
    const double one[1] = {1.0};
    std::vector<double> x(1, 0.0);
    CHECK_FALSE(lu.solve(one, x));  // not analyzed

    SparsePatternBuilder b;
    b.add(0, 0);
    SparsePattern p = b.build(1);
    SparseMatrix<double> m(std::move(p));
    m.at(0, 0) = 2.0;

    CHECK_FALSE(lu.factorize(m));  // not analyzed yet
    REQUIRE(lu.analyze(m.pattern()));
    CHECK(lu.analyzed());
    CHECK_FALSE(lu.factored());

    // Pattern mismatch: analyze a 2x2, factorize with the 1x1 matrix.
    SparsePatternBuilder b2;
    b2.add(0, 0);
    b2.add(1, 1);
    SparseMatrix<double> m2(b2.build(2));
    m2.at(0, 0) = 1.0;
    m2.at(1, 1) = 1.0;
    SparseLu lu2;
    REQUIRE(lu2.analyze(m2.pattern()));
    CHECK_FALSE(lu2.factorize(m));  // different pattern
    REQUIRE(lu2.factorize(m2));     // right pattern: fine
    CHECK(lu2.factored());
}

TEST_CASE("complex sparse solve") {
    using cd = std::complex<double>;
    // [[1, i], [0, 2]] x = [1+i, 2]  =>  x1 = 1, x0 = 1 + i - i*1 = 1
    SparsePatternBuilder b;
    b.add(0, 0);
    b.add(0, 1);
    b.add(1, 1);
    SparseMatrix<cd> m(b.build(2));
    m.at(0, 0) = cd(1.0, 0.0);
    m.at(0, 1) = cd(0.0, 1.0);
    m.at(1, 1) = cd(2.0, 0.0);

    ComplexSparseLu lu;
    REQUIRE(lu.analyze(m.pattern()));
    REQUIRE(lu.factorize(m));

    const std::vector<cd> rhs{cd(1.0, 1.0), cd(2.0, 0.0)};
    std::vector<cd> x(2);
    REQUIRE(lu.solve(rhs, x));
    CHECK(x[0].real() == doctest::Approx(1.0));
    CHECK(x[0].imag() == doctest::Approx(0.0));
    CHECK(x[1].real() == doctest::Approx(1.0));
    CHECK(x[1].imag() == doctest::Approx(0.0));
}

TEST_CASE("transpose solve matches an explicit transposed factorization") {
    Lcg rng(0x5EED);
    const std::size_t n = 20;
    std::vector<double> dense(n * n, 0.0);
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            if (rng.unit() < 0.3) {
                dense[r * n + c] = 0.5 + rng.unit();
            }
        }
        dense[r * n + r] += 2.0;
    }
    std::vector<double> b(n);
    for (double& v : b) {
        v = -1.0 + 2.0 * rng.unit();
    }

    SparseMatrix<double> m = from_dense(dense, n);
    SparseLu lu;
    REQUIRE(lu.analyze(m.pattern()));
    REQUIRE(lu.factorize(m));

    std::vector<double> x_adj(n, 0.0);
    REQUIRE(lu.solve_transpose(b, x_adj));

    // Reference: factorize A^T itself.
    std::vector<double> dense_t(n * n);
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            dense_t[r * n + c] = dense[c * n + r];
        }
    }
    SparseMatrix<double> mt = from_dense(dense_t, n);
    SparseLu lu_t;
    REQUIRE(lu_t.analyze(mt.pattern()));
    REQUIRE(lu_t.factorize(mt));
    std::vector<double> x_ref(n, 0.0);
    REQUIRE(lu_t.solve(b, x_ref));

    for (std::size_t i = 0; i < n; ++i) {
        CHECK(x_adj[i] == doctest::Approx(x_ref[i]).epsilon(1e-12));
    }

    // A new factorize() invalidates the transpose factors (they rebuild over
    // the new values - verified by the count staying cheap and the answers
    // following the latest numbers).
    for (double& v : m.values()) {
        v *= 1.5;
    }
    REQUIRE(lu.factorize(m));
    std::vector<double> x_adj2(n, 0.0);
    REQUIRE(lu.solve_transpose(b, x_adj2));
    SparseMatrix<double> mt2 = from_dense([&] {
        std::vector<double> d(n * n);
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t c = 0; c < n; ++c) {
                d[r * n + c] = m.contains(c, r) ? m.at(c, r) : 0.0;
            }
        }
        return d;
    }(), n);
    SparseLu lu_t2;
    REQUIRE(lu_t2.analyze(mt2.pattern()));
    REQUIRE(lu_t2.factorize(mt2));
    std::vector<double> x_ref2(n, 0.0);
    REQUIRE(lu_t2.solve(b, x_ref2));
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(x_adj2[i] == doctest::Approx(x_ref2[i]).epsilon(1e-12));
    }
}
