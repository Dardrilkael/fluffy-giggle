#include <ws/math/lu.hpp>

#include <stdexcept>

// Eigen lives only in this TU - it is ws::math's private implementation
// detail and must never escape into a header (workspace rule: a lib never
// leaks its dependencies into consumers).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <Eigen/Dense>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ws::math {

namespace {

// Matrix is row-major; keeping Eigen row-major too means its storage lines
// up with Matrix::data() with no transpose on the way in or out.
using RowMajorMatrix = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

}  // namespace

// Factorization is delegated to Eigen's PartialPivLU (PA = LU, unit-diagonal
// L packed under U in the returned matrix, piv[k] = original row index that
// ended up in row k). The solve below is plain forward/back substitution over
// that packing, which matches the LuFactors contract exactly.
LuFactors lu_decompose(const Matrix<double>& a) {
    if (a.rows() != a.cols()) {
        throw std::invalid_argument("lu_decompose: matrix must be square");
    }

    const Eigen::Map<const RowMajorMatrix> input(a.data(), static_cast<Eigen::Index>(a.rows()),
                                                 static_cast<Eigen::Index>(a.cols()));
    Eigen::PartialPivLU<RowMajorMatrix> plu(input);
    const RowMajorMatrix& packed = plu.matrixLU();
    if (packed.diagonal().cwiseAbs().minCoeff() == 0.0) {
        throw std::runtime_error("lu_decompose: matrix is singular");
    }

    LuFactors f;
    f.lu = Matrix<double>(a.rows(), a.cols());
    for (std::size_t r = 0; r < a.rows(); ++r) {
        for (std::size_t c = 0; c < a.cols(); ++c) {
            f.lu(r, c) = packed(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
        }
    }

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
