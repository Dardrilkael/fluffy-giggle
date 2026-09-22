#pragma once

#include <cstddef>
#include <vector>

#include <ws/math/matrix.hpp>

namespace ws::math {

// Result of LU decomposition with partial pivoting: A = P * L * U where
// L is unit lower triangular, U is upper triangular, both packed into `lu`
// (L's diagonal is implicit 1s). `piv[k]` is the original row index that was
// swapped into row k during factorization.
struct LuFactors {
    Matrix<double> lu;
    std::vector<std::size_t> piv;
};

// Factors a square matrix. Throws:
//   std::invalid_argument - if a is not square
//   std::runtime_error    - if a is singular (zero pivot column)
LuFactors lu_decompose(const Matrix<double>& a);

// Solves A x = b using a prior lu_decompose result.
// Throws std::invalid_argument if b has the wrong size.
std::vector<double> lu_solve(const LuFactors& f, const std::vector<double>& b);

}  // namespace ws::math
