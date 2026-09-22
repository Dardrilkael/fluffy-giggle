#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <vector>

// Eigen is a third-party header - suppress anything MSVC might warn about.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Dense>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <ws/core/assert.hpp>

namespace ws::math {

// Dense row-major matrix, backed by Eigen (pinned 3.4.0, wrapped so the rest
// of the workspace only ever sees this API). Storage layout is row-major:
// data()[r * cols() + c].
template <typename T>
class Matrix {
public:
    using EigenMatrix = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

    Matrix() = default;

    Matrix(std::size_t rows, std::size_t cols, T init = T{}) : eigen_(rows, cols) {
        if (rows == 0 || cols == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        eigen_.setConstant(init);
    }

    // Matrix m = {{1, 2, 3},
    //             {4, 5, 6}};
    Matrix(std::initializer_list<std::initializer_list<T>> rows) {
        if (rows.size() == 0 || rows.begin()->size() == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        const std::size_t ncols = rows.begin()->size();
        eigen_.resize(rows.size(), ncols);
        std::size_t r = 0;
        for (const auto& row : rows) {
            if (row.size() != ncols) {
                throw std::invalid_argument("All matrix rows must have the same length");
            }
            std::size_t c = 0;
            for (const T& v : row) {
                eigen_(r, c++) = v;
            }
            ++r;
        }
    }

    std::size_t rows() const { return static_cast<std::size_t>(eigen_.rows()); }
    std::size_t cols() const { return static_cast<std::size_t>(eigen_.cols()); }

    // Fast element access. Bounds are checked in debug builds (WS_CORE_ASSERT).
    T& operator()(std::size_t r, std::size_t c) {
        WS_CORE_ASSERT(r < rows() && c < cols());
        return eigen_(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
    }
    const T& operator()(std::size_t r, std::size_t c) const {
        WS_CORE_ASSERT(r < rows() && c < cols());
        return eigen_(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
    }

    // Always-checked element access (throws std::out_of_range).
    T& at(std::size_t r, std::size_t c) {
        if (r >= rows() || c >= cols()) {
            throw std::out_of_range("Matrix index out of range");
        }
        return (*this)(r, c);
    }
    const T& at(std::size_t r, std::size_t c) const {
        if (r >= rows() || c >= cols()) {
            throw std::out_of_range("Matrix index out of range");
        }
        return (*this)(r, c);
    }

    // Contiguous row-major storage.
    const T* data() const { return eigen_.data(); }

    // Escape hatch to the Eigen matrix, for anything this wrapper does not
    // expose (decompositions, reductions, expressions, ...).
    const EigenMatrix& eigen() const { return eigen_; }
    EigenMatrix& eigen() { return eigen_; }

    static Matrix zeros(std::size_t rows, std::size_t cols) { return Matrix(rows, cols, T{}); }
    static Matrix ones(std::size_t rows, std::size_t cols) { return Matrix(rows, cols, T{1}); }
    static Matrix identity(std::size_t n) {
        Matrix m(n, n);
        m.eigen_.setIdentity();
        return m;
    }

    Matrix transpose() const {
        Matrix t;
        t.eigen_ = eigen_.transpose();
        return t;
    }

    Matrix operator*(const Matrix& rhs) const {
        if (cols() != rhs.rows()) {
            throw std::invalid_argument("Matrix multiply: inner dimensions must match");
        }
        Matrix out;
        out.eigen_ = eigen_ * rhs.eigen_;
        return out;
    }

    // Matrix-vector product, result has this->rows() entries.
    std::vector<T> operator*(const std::vector<T>& v) const {
        if (v.size() != cols()) {
            throw std::invalid_argument("Matrix-vector product: dimension mismatch");
        }
        const Eigen::Map<const Eigen::Matrix<T, Eigen::Dynamic, 1>> mapped(v.data(),
                                                                           static_cast<Eigen::Index>(v.size()));
        const Eigen::Matrix<T, Eigen::Dynamic, 1> out = eigen_ * mapped;
        return std::vector<T>(out.data(), out.data() + out.size());
    }

    Matrix operator+(const Matrix& rhs) const {
        if (rows() != rhs.rows() || cols() != rhs.cols()) {
            throw std::invalid_argument("Matrix add: dimensions must match");
        }
        Matrix out;
        out.eigen_ = eigen_ + rhs.eigen_;
        return out;
    }

    Matrix operator*(T scalar) const {
        Matrix out;
        out.eigen_ = eigen_ * scalar;
        return out;
    }

private:
    EigenMatrix eigen_;
};

}  // namespace ws::math
