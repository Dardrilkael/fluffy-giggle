# libs/math - CLAUDE.md

All math in the workspace. Downstream code uses ws::math names only - and
sees no third-party headers at all: everything under include/ is ws-owned.

## Purpose
- `Matrix<T>` (`ws/math/matrix.hpp`): dense, row-major, dynamically-sized,
  over a flat std::vector. Multiply, transpose, add, scalar, matrix-vector
  product.
- LU decomposition + solve (`ws/math/lu.hpp`): the factorization runs on
  Eigen's PartialPivLU - but only inside `src/lu.cpp`.
- `vecmath.hpp`: fixed-size graphics math - `Vec2/3/4`, `DVec*`, `IVec*`,
  `Mat2/3/4`, `Quat`, plus `dot/cross/normalize/length/...` - all ws-owned.
- Sparse LU with symbolic reuse (`ws/math/sparse.hpp`, grew out of the
  ws::spice arc, DESIGN.md §4): `SparsePatternBuilder` freezes stamped
  (row,col) pairs into a CSC `SparsePattern`; `SparseMatrix<T>` holds the
  numeric values over that pattern (`slot()/at()/add()`, an out-of-pattern
  write is a WS_CORE_ASSERT programmer error); `SparseLu` /
  `ComplexSparseLu` do analyze-once / factorize-many / solve, plus
  `solve_transpose` (noise adjoint). Backend is Eigen's SparseLU + COLAMD,
  PRIVATE in `src/sparse.cpp` (the KLU swap in M0b touches that file only);
  the facade is KLU-shaped so the swap is invisible to spice. Errors come
  back as `ws::core::Status` - singularity included, never an exception.

## The split (important)
- Sizes known at compile time -> vecmath.hpp types.
- Sizes decided at runtime -> `Matrix<T>`.
- Both headers may be included in the same TU (tested).

## Allowed dependencies
- `ws::core` (asserts only) and Eigen - but Eigen is PRIVATE: only
  `src/lu.cpp` includes it, and nothing under include/ ever may. Never GUI
  headers. (GLM was dropped when the no-leak rule landed - type aliases
  cannot hide their backend.)

## Public API
- `ws::math::Matrix<T>`: `operator()(r, c)` (assert-checked in debug),
  `at(r, c)` (throws), `zeros/ones/identity`, `transpose`, `operator*`
  (matmul, matvec, scalar), `operator+`, `data()`.
- `ws::math::lu_decompose(Matrix<double>) -> LuFactors`,
  `ws::math::lu_solve(LuFactors, b) -> std::vector<double>`.
- `ws::math::SparsePatternBuilder::build(n) -> SparsePattern`,
  `SparseMatrix<T>::slot/at/add/zero/values`, `SparseLu::analyze/
  factorize/solve/solve_transpose -> ws::core::Status` (same shape for
  `ComplexSparseLu` over `std::complex<double>`). `factorization_count()`
  feeds caller diagnostics.
- `ws::math::Vec2/3/4`, `DVec*`, `IVec*`, `Mat2/3/4`, `Quat`, and the free
  functions (dot, cross, length, normalize, distance, mix, radians,
  degrees). Vectors are aggregates (`Vec3{1, 2, 3}`) that zero-fill by
  default; `Mat4 m{1.0f}` is a diagonal matrix; `rotate(Quat, Vec3)`
  applies a rotation.

## Invariants / rules
- Error handling: public APIs return `ws::core::Result`/`Status` (root
  CLAUDE.md, "Error handling") — lib note: `Matrix::at()` and
  `lu_decompose`/`lu_solve` currently throw std exceptions; they predate the
  rule and convert when next touched.
- No third-party header may appear under include/ - if Eigen-level numerics
  are needed, add a ws::math function instead of exposing Eigen.
- Matrix storage is row-major: `data()[r * cols() + c]`.
- vecmath Mat types are column-major (OpenGL convention) - do not confuse
  the two.
- Error handling: dimension errors throw `std::invalid_argument`, singular
  matrices throw `std::runtime_error`. lu_decompose requires square input.
- `LuFactors::piv[k]` = original row index swapped into row k.

## Tests
```
ctest --preset debug -R math
```
