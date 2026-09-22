#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <stdexcept>
#include <vector>

#include <ws/math/lu.hpp>
#include <ws/math/matrix.hpp>

using ws::math::Matrix;

TEST_CASE("matrix construction and element access") {
    const Matrix<double> m = {{1.0, 2.0, 3.0},
                             {4.0, 5.0, 6.0}};
    CHECK(m.rows() == 2);
    CHECK(m.cols() == 3);
    CHECK(m(0, 1) == 2.0);
    CHECK(m(1, 2) == 6.0);

    const Matrix<double> t = m.transpose();
    CHECK(t.rows() == 3);
    CHECK(t.cols() == 2);
    CHECK(t(2, 0) == 3.0);
    CHECK(t(1, 1) == 5.0);

    Matrix<int> z = Matrix<int>::zeros(2, 2);
    CHECK(z(1, 1) == 0);

    const auto id = Matrix<double>::identity(3);
    CHECK(id(0, 0) == 1.0);
    CHECK(id(0, 1) == 0.0);
    CHECK(id(2, 2) == 1.0);
}

TEST_CASE("malformed initializer lists are rejected") {
    CHECK_THROWS((Matrix<double>({{1.0, 2.0}, {3.0}})));
    CHECK_THROWS_AS((Matrix<double>({})), std::invalid_argument);
}

TEST_CASE("matrix multiplication") {
    const Matrix<double> a = {{1.0, 2.0},
                             {3.0, 4.0}};
    const Matrix<double> b = {{5.0, 6.0},
                              {7.0, 8.0}};
    const Matrix<double> c = a * b;
    CHECK(c(0, 0) == doctest::Approx(19.0));
    CHECK(c(0, 1) == doctest::Approx(22.0));
    CHECK(c(1, 0) == doctest::Approx(43.0));
    CHECK(c(1, 1) == doctest::Approx(50.0));

    // Non-square: (2x3) * (3x2) -> 2x2
    const Matrix<double> r = {{1.0, 2.0, 3.0},
                             {4.0, 5.0, 6.0}};
    const Matrix<double> s = {{7.0, 8.0},
                              {9.0, 10.0},
                              {11.0, 12.0}};
    const Matrix<double> rs = r * s;
    CHECK(rs.rows() == 2);
    CHECK(rs.cols() == 2);
    CHECK(rs(0, 0) == doctest::Approx(58.0));
    CHECK(rs(1, 1) == doctest::Approx(154.0));

    CHECK_THROWS_AS(a * s, std::invalid_argument);
}

TEST_CASE("matrix-vector product") {
    const Matrix<double> m = {{2.0, 0.0},
                             {0.0, 3.0}};
    const std::vector<double> v = {1.0, 2.0};
    const auto out = m * v;
    REQUIRE(out.size() == 2);
    CHECK(out[0] == doctest::Approx(2.0));
    CHECK(out[1] == doctest::Approx(6.0));

    CHECK_THROWS_AS(m * std::vector<double>{1.0}, std::invalid_argument);
}

TEST_CASE("scalar multiply and matrix addition") {
    const Matrix<double> a = {{1.0, 2.0},
                              {3.0, 4.0}};
    const Matrix<double> scaled = a * 10.0;
    CHECK(scaled(1, 0) == doctest::Approx(30.0));

    const Matrix<double> sum = a + a;
    CHECK(sum(0, 1) == doctest::Approx(4.0));
    CHECK(sum(1, 1) == doctest::Approx(8.0));
}

TEST_CASE("LU solve on a 2x2 system") {
    const Matrix<double> a = {{2.0, 1.0},
                              {1.0, 3.0}};
    const auto f = ws::math::lu_decompose(a);
    const auto x = ws::math::lu_solve(f, {3.0, 5.0});
    REQUIRE(x.size() == 2);
    CHECK(x[0] == doctest::Approx(0.8).epsilon(1e-12));
    CHECK(x[1] == doctest::Approx(1.4).epsilon(1e-12));
}

TEST_CASE("LU solve with pivoting (zero in top-left corner)") {
    const Matrix<double> a = {{0.0, 1.0, 1.0},
                              {1.0, 0.0, 1.0},
                              {1.0, 1.0, 0.0}};
    const auto f = ws::math::lu_decompose(a);
    const auto x = ws::math::lu_solve(f, {2.0, 3.0, 4.0});
    REQUIRE(x.size() == 3);
    CHECK(x[0] == doctest::Approx(2.5).epsilon(1e-12));
    CHECK(x[1] == doctest::Approx(1.5).epsilon(1e-12));
    CHECK(x[2] == doctest::Approx(0.5).epsilon(1e-12));
}

TEST_CASE("LU solve on a 4x4 system, then verify residual") {
    const Matrix<double> a = {{4.0, -1.0, 0.0, 0.0},
                              {-1.0, 4.0, -1.0, 0.0},
                              {0.0, -1.0, 4.0, -1.0},
                              {0.0, 0.0, -1.0, 4.0}};
    const std::vector<double> b = {1.0, 2.0, 3.0, 4.0};
    const auto x = ws::math::lu_solve(ws::math::lu_decompose(a), b);
    const auto ax = a * x;
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(ax[i] == doctest::Approx(b[i]).epsilon(1e-10));
    }
}

TEST_CASE("LU errors: non-square input and singular matrix") {
    CHECK_THROWS_AS(ws::math::lu_decompose(Matrix<double>(2, 3)), std::invalid_argument);
    CHECK_THROWS_AS(ws::math::lu_decompose(Matrix<double>({{1.0, 2.0}, {2.0, 4.0}})),
                    std::runtime_error);
}

TEST_CASE("lu_solve rejects wrong-size b") {
    const auto f = ws::math::lu_decompose(Matrix<double>::identity(2));
    CHECK_THROWS_AS(ws::math::lu_solve(f, {1.0, 2.0, 3.0}), std::invalid_argument);
}
