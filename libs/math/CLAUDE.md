# libs/math - CLAUDE.md

All math in the workspace, wrapped from two pinned third-party backends.
Downstream code uses ws::math names only - never raw Eigen:: or glm:: names.

## Purpose
- `Matrix<T>` (`ws/math/matrix.hpp`): dense, row-major, dynamically-sized.
  Multiply, transpose, add, scalar, matrix-vector product.
- LU decomposition + solve (`ws/math/lu.hpp`): PartialPivLU-backed.
- `vecmath.hpp`: fixed-size graphics math - `Vec2/3/4`, `DVec*`, `IVec*`,
  `Mat2/3/4`, `Quat`, plus `dot/cross/normalize/length/...` - GLM-backed.

## Backends and the split (important)
- **Eigen 3.4.0** backs matrix.hpp/lu.hpp (dynamic dense numerics).
- **GLM 1.0.1** backs vecmath.hpp (compile-time-sized graphics math).
- Rule of thumb: sizes known at compile time -> vecmath.hpp types;
  sizes decided at runtime -> Matrix<T>.
- Both headers may be included in the same TU (tested).

## Allowed dependencies
- `ws::core` (asserts only), Eigen, GLM. Nothing else. Never GUI headers.

## Public API
- `ws::math::Matrix<T>`: `operator()(r, c)` (assert-checked in debug),
  `at(r, c)` (throws), `zeros/ones/identity`, `transpose`, `operator*`
  (matmul, matvec, scalar), `operator+`, `data()`, `eigen()` escape hatch.
- `ws::math::lu_decompose(Matrix<double>) -> LuFactors`,
  `ws::math::lu_solve(LuFactors, b) -> std::vector<double>`.
- `ws::math::Vec2/3/4`, `DVec*`, `IVec*`, `Mat2/3/4`, `Quat` and the
  re-exported `glm` functions (dot, cross, normalize, ...).

## Invariants / rules
- Matrix storage is row-major: `data()[r * cols() + c]`.
- GLM matrices are column-major (GLM convention) - do not confuse the two.
- Error handling: dimension errors throw `std::invalid_argument`, singular
  matrices throw `std::runtime_error`. lu_decompose requires square input.
- `LuFactors::piv[k]` = original row index swapped into row k.

## Tests
```
ctest --preset debug -R math
```
