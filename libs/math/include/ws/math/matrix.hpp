#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include <ws/core/assert.hpp>

namespace ws::math {

// Dense row-major matrix. data_[r * cols_ + c].
template <typename T>
class Matrix {
public:
    Matrix() = default;

    Matrix(std::size_t rows, std::size_t cols, T init = T{})
        : rows_(rows), cols_(cols), data_(rows * cols, init) {
        if (rows == 0 || cols == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
    }

    // Matrix m = {{1, 2, 3},
    //             {4, 5, 6}};
    Matrix(std::initializer_list<std::initializer_list<T>> rows) {
        rows_ = rows.size();
        if (rows_ == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        cols_ = rows.begin()->size();
        if (cols_ == 0) {
            throw std::invalid_argument("Matrix dimensions must be > 0");
        }
        data_.reserve(rows_ * cols_);
        for (const auto& row : rows) {
            if (row.size() != cols_) {
                throw std::invalid_argument("All matrix rows must have the same length");
            }
            data_.insert(data_.end(), row.begin(), row.end());
        }
    }

    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }

    // Fast element access. Bounds are checked in debug builds (WS_CORE_ASSERT).
    T& operator()(std::size_t r, std::size_t c) {
        WS_CORE_ASSERT(r < rows_ && c < cols_);
        return data_[r * cols_ + c];
    }
    const T& operator()(std::size_t r, std::size_t c) const {
        WS_CORE_ASSERT(r < rows_ && c < cols_);
        return data_[r * cols_ + c];
    }

    // Always-checked element access (throws std::out_of_range).
    T& at(std::size_t r, std::size_t c) {
        if (r >= rows_ || c >= cols_) {
            throw std::out_of_range("Matrix index out of range");
        }
        return data_[r * cols_ + c];
    }
    const T& at(std::size_t r, std::size_t c) const {
        if (r >= rows_ || c >= cols_) {
            throw std::out_of_range("Matrix index out of range");
        }
        return data_[r * cols_ + c];
    }

    const T* data() const { return data_.data(); }
    const std::vector<T>& storage() const { return data_; }

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
        Matrix t(cols_, rows_);
        for (std::size_t r = 0; r < rows_; ++r) {
            for (std::size_t c = 0; c < cols_; ++c) {
                t(c, r) = (*this)(r, c);
            }
        }
        return t;
    }

    Matrix operator*(const Matrix& rhs) const {
        if (cols_ != rhs.rows_) {
            throw std::invalid_argument("Matrix multiply: inner dimensions must match");
        }
        Matrix out(rows_, rhs.cols_);
        for (std::size_t r = 0; r < rows_; ++r) {
            for (std::size_t k = 0; k < cols_; ++k) {
                const T a = (*this)(r, k);
                if (a == T{}) {
                    continue;  // skip zero factors (keeps sparse-ish matrices cheap)
                }
                for (std::size_t c = 0; c < rhs.cols_; ++c) {
                    out(r, c) += a * rhs(k, c);
                }
            }
        }
        return out;
    }

    // Matrix-vector product, result has this->rows() entries.
    std::vector<T> operator*(const std::vector<T>& v) const {
        if (cols_ != v.size()) {
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
        if (rows_ != rhs.rows_ || cols_ != rhs.cols_) {
            throw std::invalid_argument("Matrix add: dimensions must match");
        }
        Matrix out(rows_, cols_);
        for (std::size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] + rhs.data_[i];
        }
        return out;
    }

    Matrix operator*(T scalar) const {
        Matrix out(rows_, cols_);
        for (std::size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] * scalar;
        }
        return out;
    }

private:
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    std::vector<T> data_;
};

}  // namespace ws::math
