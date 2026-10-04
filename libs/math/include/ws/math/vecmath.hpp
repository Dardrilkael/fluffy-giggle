#pragma once

// Fixed-size vector / matrix / quaternion math for graphics and geometry.
// Everything here is ws-owned - no third-party header is included, so
// consumers compile nothing but this file (workspace rule: a lib never leaks
// its dependencies into consumers).
//
// Division of labor inside ws::math:
//   - matrix.hpp / lu.hpp  -> dynamically-sized dense numerics
//   - vecmath.hpp          -> compile-time-sized graphics math (this file)
// Use these types when sizes are known at compile time.

#include <cmath>
#include <cstddef>
#include <numbers>
#include <type_traits>

namespace ws::math {

// --- vectors ---------------------------------------------------------------
// Aggregates: Vec3 v{1, 2, 3} sets components, Vec3 v; zero-fills.

template <typename T, int N>
struct Vec;  // only the 2/3/4 specializations below are defined

template <typename T>
struct Vec<T, 2> {
    T x{}, y{};

    T& operator[](std::size_t i) { return i == 0 ? x : y; }
    const T& operator[](std::size_t i) const { return i == 0 ? x : y; }
};

template <typename T>
struct Vec<T, 3> {
    T x{}, y{}, z{};

    T& operator[](std::size_t i) {
        switch (i) {
        case 0: return x;
        case 1: return y;
        default: return z;
        }
    }
    const T& operator[](std::size_t i) const {
        switch (i) {
        case 0: return x;
        case 1: return y;
        default: return z;
        }
    }
};

template <typename T>
struct Vec<T, 4> {
    T x{}, y{}, z{}, w{};

    T& operator[](std::size_t i) {
        switch (i) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }
    const T& operator[](std::size_t i) const {
        switch (i) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }
};

using Vec2 = Vec<float, 2>;
using Vec3 = Vec<float, 3>;
using Vec4 = Vec<float, 4>;

using DVec2 = Vec<double, 2>;
using DVec3 = Vec<double, 3>;
using DVec4 = Vec<double, 4>;

using IVec2 = Vec<int, 2>;
using IVec3 = Vec<int, 3>;
using IVec4 = Vec<int, 4>;

// --- vector operators ------------------------------------------------------

template <typename T, int N>
Vec<T, N> operator+(const Vec<T, N>& a, const Vec<T, N>& b) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = a[i] + b[i];
    }
    return out;
}

template <typename T, int N>
Vec<T, N> operator-(const Vec<T, N>& a, const Vec<T, N>& b) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = a[i] - b[i];
    }
    return out;
}

// Component-wise product.
template <typename T, int N>
Vec<T, N> operator*(const Vec<T, N>& a, const Vec<T, N>& b) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = a[i] * b[i];
    }
    return out;
}

template <typename T, int N>
Vec<T, N> operator*(const Vec<T, N>& v, T s) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = v[i] * s;
    }
    return out;
}

template <typename T, int N>
Vec<T, N> operator*(T s, const Vec<T, N>& v) {
    return v * s;
}

template <typename T, int N>
Vec<T, N> operator/(const Vec<T, N>& v, T s) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = v[i] / s;
    }
    return out;
}

template <typename T, int N>
Vec<T, N> operator-(const Vec<T, N>& v) {
    Vec<T, N> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out[i] = -v[i];
    }
    return out;
}

template <typename T, int N>
bool operator==(const Vec<T, N>& a, const Vec<T, N>& b) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        if (!(a[i] == b[i])) {
            return false;
        }
    }
    return true;
}

template <typename T, int N>
bool operator!=(const Vec<T, N>& a, const Vec<T, N>& b) {
    return !(a == b);
}

// --- vector functions ------------------------------------------------------

template <typename T, int N>
T dot(const Vec<T, N>& a, const Vec<T, N>& b) {
    T out{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
        out += a[i] * b[i];
    }
    return out;
}

template <typename T>
Vec<T, 3> cross(const Vec<T, 3>& a, const Vec<T, 3>& b) {
    return Vec<T, 3>{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename T, int N>
T length(const Vec<T, N>& v) {
    static_assert(std::is_floating_point_v<T>, "length() needs a floating-point vector");
    return std::sqrt(dot(v, v));
}

// Zero-length input is undefined (yields inf/NaN, like GLM).
template <typename T, int N>
Vec<T, N> normalize(const Vec<T, N>& v) {
    static_assert(std::is_floating_point_v<T>, "normalize() needs a floating-point vector");
    return v * (T{1} / length(v));
}

template <typename T, int N>
T distance(const Vec<T, N>& a, const Vec<T, N>& b) {
    static_assert(std::is_floating_point_v<T>, "distance() needs floating-point vectors");
    return length(b - a);
}

// Linear interpolation: t == 0 -> a, t == 1 -> b.
template <typename T, int N>
Vec<T, N> mix(const Vec<T, N>& a, const Vec<T, N>& b, T t) {
    static_assert(std::is_floating_point_v<T>, "mix() needs floating-point vectors");
    return a + (b - a) * t;
}

template <typename T>
T mix(T a, T b, T t) {
    static_assert(std::is_floating_point_v<T>, "mix() needs floating-point scalars");
    return a + (b - a) * t;
}

// Angle helpers (radians).
template <typename T>
constexpr T radians(T deg) {
    static_assert(std::is_floating_point_v<T>, "radians() needs a floating-point scalar");
    return deg * std::numbers::pi_v<T> / T{180};
}

template <typename T>
constexpr T degrees(T rad) {
    static_assert(std::is_floating_point_v<T>, "degrees() needs a floating-point scalar");
    return rad * T{180} / std::numbers::pi_v<T>;
}

// --- matrices ---------------------------------------------------------------
// Column-major (OpenGL convention): c[i] is the i-th column, and the element
// at row r of column i is c[i][r].

template <typename T, int N>
struct Mat {
    Vec<T, N> c[N]{};

    Mat() = default;  // zero matrix

    // Mat4 m{1.0f} is a diagonal matrix - the OpenGL/GLM habit.
    explicit Mat(T diagonal) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
            c[i][i] = diagonal;
        }
    }

    static Mat identity() { return Mat(T{1}); }

    Vec<T, N>& operator[](std::size_t i) { return c[i]; }
    const Vec<T, N>& operator[](std::size_t i) const { return c[i]; }
};

using Mat2 = Mat<float, 2>;
using Mat3 = Mat<float, 3>;
using Mat4 = Mat<float, 4>;

using DMat3 = Mat<double, 3>;
using DMat4 = Mat<double, 4>;

// m * v: out[r] = sum over columns of m[col][r] * v[col].
template <typename T, int N>
Vec<T, N> operator*(const Mat<T, N>& m, const Vec<T, N>& v) {
    Vec<T, N> out{};
    for (std::size_t r = 0; r < static_cast<std::size_t>(N); ++r) {
        T sum{};
        for (std::size_t col = 0; col < static_cast<std::size_t>(N); ++col) {
            sum += m[col][r] * v[col];
        }
        out[r] = sum;
    }
    return out;
}

// Standard column-major matrix product.
template <typename T, int N>
Mat<T, N> operator*(const Mat<T, N>& a, const Mat<T, N>& b) {
    Mat<T, N> out{};
    for (std::size_t col = 0; col < static_cast<std::size_t>(N); ++col) {
        for (std::size_t r = 0; r < static_cast<std::size_t>(N); ++r) {
            T sum{};
            for (std::size_t k = 0; k < static_cast<std::size_t>(N); ++k) {
                sum += a[k][r] * b[col][k];
            }
            out[col][r] = sum;
        }
    }
    return out;
}

// --- quaternions ------------------------------------------------------------

struct Quat {
    float x{}, y{}, z{};
    float w{1.0f};  // default construction is the identity rotation
};

// Hamilton product.
inline Quat operator*(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Zero-length input is undefined (yields inf/NaN, like GLM and normalize()).
inline Quat normalize(const Quat& q) {
    const float inv = 1.0f / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Rotates v by q (q must be normalized).
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 axis{q.x, q.y, q.z};
    const Vec3 t = cross(axis, v) * 2.0f;
    return v + t * q.w + cross(axis, t);
}

}  // namespace ws::math
