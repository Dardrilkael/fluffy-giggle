#include <ws/math/lu.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace ws::math {

LuFactors lu_decompose(const Matrix<double>& a) {
    if (a.rows() != a.cols()) {
        throw std::invalid_argument("lu_decompose: matrix must be square");
    }
    const std::size_t n = a.rows();

    LuFactors f;
    f.lu = a;
    f.piv.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        f.piv[i] = i;
    }

    for (std::size_t k = 0; k < n; ++k) {
        // Partial pivoting: find the row i >= k with the largest |lu(i, k)|.
        std::size_t best = k;
        double best_abs = std::fabs(f.lu(k, k));
        for (std::size_t i = k + 1; i < n; ++i) {
            const double v = std::fabs(f.lu(i, k));
            if (v > best_abs) {
                best_abs = v;
                best = i;
            }
        }

        if (best_abs == 0.0) {
            throw std::runtime_error("lu_decompose: matrix is singular");
        }

        if (best != k) {
            std::swap(f.piv[k], f.piv[best]);
            for (std::size_t c = 0; c < n; ++c) {
                std::swap(f.lu(k, c), f.lu(best, c));
            }
        }

        for (std::size_t i = k + 1; i < n; ++i) {
            const double factor = f.lu(i, k) / f.lu(k, k);
            f.lu(i, k) = factor;
            for (std::size_t j = k + 1; j < n; ++j) {
                f.lu(i, j) -= factor * f.lu(k, j);
            }
        }
    }
    return f;
}

std::vector<double> lu_solve(const LuFactors& f, const std::vector<double>& b) {
    const std::size_t n = f.lu.rows();
    if (b.size() != n) {
        throw std::invalid_argument("lu_solve: b size must match matrix size");
    }

    // Apply the row permutation, then forward substitution (L, unit diagonal).
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = b[f.piv[i]];
        for (std::size_t j = 0; j < i; ++j) {
            sum -= f.lu(i, j) * y[j];
        }
        y[i] = sum;
    }

    // Back substitution (U).
    std::vector<double> x(n);
    for (std::size_t ii = n; ii-- > 0;) {
        double sum = y[ii];
        for (std::size_t j = ii + 1; j < n; ++j) {
            sum -= f.lu(ii, j) * x[j];
        }
        x[ii] = sum / f.lu(ii, ii);
    }
    return x;
}

}  // namespace ws::math
