#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include <ws/core/assert.hpp>

namespace ws::math {

// Dense row-major matrix over a flat std::vector. Deliberately free of
// third-party types (workspace rule: a lib never leaks its dependencies
// into consumers) - anything numerics-heavy lives in src/ instead.
// Storage layout: data()[r * cols() + c].
template <typename T>
class Matrix {
public:
    Matrix() = default;

    Matrix(std::size_t rows, std::size_t cols, T init = T{}) : rows_(rows), cols_(cols) {
        if (rows == 0 || cols == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        data_.assign(rows * cols, init);
    }

    // Matrix m = {{1, 2, 3},
    //             {4, 5, 6}};
    Matrix(std::initializer_list<std::initializer_list<T>> rows) {
        if (rows.size() == 0 || rows.begin()->size() == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        const std::size_t ncols = rows.begin()->size();
        for (const auto& row : rows) {
            if (row.size() != ncols) {
                throw std::invalid_argument("All matrix rows must have the same length");
            }
        }
        rows_ = rows.size();
        cols_ = ncols;
        data_.reserve(rows_ * cols_);
        for (const auto& row : rows) {
            data_.insert(data_.end(), row.begin(), row.end());
        }
    }

    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }

    // Fast element access. Bounds are checked in debug builds (WS_CORE_ASSERT).
    T& operator()(std::size_t r, std::size_t c) {
        WS_CORE_ASSERT(r < rows() && c < cols());
        return data_[r * cols_ + c];
    }
    const T& operator()(std::size_t r, std::size_t c) const {
        WS_CORE_ASSERT(r < rows() && c < cols());
        return data_[r * cols_ + c];
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
    const T* data() const { return data_.data(); }

    static Matrix zeros(std::size_t rows, std::size_t cols) { return Matrix(rows, cols, T{}); }
    static Matrix ones(std::size_t rows, std::size_t cols) { return Matrix(rows, cols, T{1}); }
    static Matrix identity(std::size_t n) {
        Matrix m(n, n);
        for (std::size_t i = 0; i < n; ++i) {
            m(i, i) = T{1};
        }
        return m;
    }

    Matrix transpose() const {
        if (rows_ == 0) {
            return Matrix{};
        }
        Matrix t(cols_, rows_);
        for (std::size_t r = 0; r < rows_; ++r) {
            for (std::size_t c = 0; c < cols_; ++c) {
                t(c, r) = (*this)(r, c);
            }
        }
        return t;
    }

    Matrix operator*(const Matrix& rhs) const {
        if (cols() != rhs.rows()) {
            throw std::invalid_argument("Matrix multiply: inner dimensions must match");
        }
        Matrix out;
        out.rows_ = rows_;
        out.cols_ = rhs.cols_;
        out.data_.assign(rows_ * rhs.cols_, T{});
        // i/k/j loop order: both operands stream through row-major cache
        // lines without transposing either one.
        for (std::size_t i = 0; i < rows_; ++i) {
            for (std::size_t k = 0; k < cols_; ++k) {
                const T a = (*this)(i, k);
                for (std::size_t j = 0; j < rhs.cols_; ++j) {
                    out(i, j) += a * rhs(k, j);
                }
            }
        }
        return out;
    }

    // Matrix-vector product, result has this->rows() entries.
    std::vector<T> operator*(const std::vector<T>& v) const {
        if (v.size() != cols()) {
            throw std::invalid_argument("Matrix-vector product: dimension mismatch");
        }
        std::vector<T> out(rows_, T{});
        for (std::size_t r = 0; r < rows_; ++r) {
            T sum{};
            for (std::size_t c = 0; c < cols_; ++c) {
                sum += (*this)(r, c) * v[c];
            }
            out[r] = sum;
        }
        return out;
    }

    Matrix operator+(const Matrix& rhs) const {
        if (rows() != rhs.rows() || cols() != rhs.cols()) {
            throw std::invalid_argument("Matrix add: dimensions must match");
        }
        Matrix out = *this;
        for (std::size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] += rhs.data_[i];
        }
        return out;
    }

    Matrix operator*(T scalar) const {
        Matrix out = *this;
        for (T& v : out.data_) {
            v *= scalar;
        }
        return out;
    }

private:
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    std::vector<T> data_;
};

}  // namespace ws::math
