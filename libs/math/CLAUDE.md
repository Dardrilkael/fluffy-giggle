# libs/math - CLAUDE.md

Dense linear algebra. Pure numerics: no I/O, no GUI, nothing platform-specific.

## Purpose
- `Matrix<T>` (`ws/math/matrix.hpp`): dense, row-major matrix with multiply,
  transpose, add, scalar, matrix-vector product.
- LU decomposition with partial pivoting and solve (`ws/math/lu.hpp`).

## Allowed dependencies
- `ws::core` (asserts only).
- Standard library. Nothing else. Never GUI headers of any kind.

## Public API
- `ws::math::Matrix<T>`: `operator()(r, c)` (assert-checked in debug),
  `at(r, c)` (throws), `zeros/ones/identity`, `transpose`, `operator*`
  (matmul, matvec, scalar), `operator+`, `data()`, `storage()`.
- `ws::math::lu_decompose(Matrix<double>) -> LuFactors` and
  `ws::math::lu_solve(LuFactors, b) -> std::vector<double>`.

## Invariants / rules
- Storage is row-major: `data()[r * cols() + c]`.
- Error handling: dimension errors throw `std::invalid_argument`,
  singular matrices throw `std::runtime_error`. lu_decompose requires square
  input.
- LU pivots on the largest |a(i,k)|; `LuFactors::piv` records row swaps.
- double precision only for LU (Matrix<T> is generic, LU is double-specialized).

## Tests
```
ctest --preset debug -R math
```
