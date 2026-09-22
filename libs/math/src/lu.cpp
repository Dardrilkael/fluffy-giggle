#include <ws/math/lu.hpp>

#include <stdexcept>
#include <utility>

namespace ws::math {

// Factorization is delegated to Eigen's PartialPivLU (PA = LU, unit-diagonal
// L packed under U in the returned matrix, piv[k] = original row index that
// ended up in row k). The solve below is plain forward/back substitution over
// that packing, which matches the LuFactors contract exactly.
LuFactors lu_decompose(const Matrix<double>& a) {
    if (a.rows() != a.cols()) {
        throw std::invalid_argument("lu_decompose: matrix must be square");
    }

    Eigen::PartialPivLU<Eigen::MatrixXd> plu(a.eigen());
    const Eigen::MatrixXd& packed = plu.matrixLU();
    if (packed.diagonal().cwiseAbs().minCoeff() == 0.0) {
        throw std::runtime_error("lu_decompose: matrix is singular");
    }

    LuFactors f;
    f.lu = Matrix<double>(a.rows(), a.cols());
    f.lu.eigen() = packed;

    const auto& perm = plu.permutationP().indices();
    f.piv.assign(perm.data(), perm.data() + perm.size());
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
