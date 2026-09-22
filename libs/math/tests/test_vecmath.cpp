// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_math.cpp owns doctest's
// main() for the math test binary.

// Deliberately includes BOTH the Eigen-backed and GLM-backed headers in one
// translation unit: they must coexist cleanly.
#include <ws/math/lu.hpp>
#include <ws/math/matrix.hpp>
#include <ws/math/vecmath.hpp>

#include <doctest/doctest.h>

TEST_CASE("vec3 basics: length, dot, normalize") {
    const ws::math::Vec3 v{3.0f, 4.0f, 0.0f};
    CHECK(ws::math::length(v) == doctest::Approx(5.0f));
    CHECK(ws::math::dot(v, ws::math::Vec3{1.0f, 0.0f, 0.0f}) == doctest::Approx(3.0f));

    const ws::math::Vec3 n = ws::math::normalize(v);
    CHECK(ws::math::length(n) == doctest::Approx(1.0f).epsilon(1e-6f));
}

TEST_CASE("vec3 cross product") {
    const ws::math::Vec3 x{1.0f, 0.0f, 0.0f};
    const ws::math::Vec3 y{0.0f, 1.0f, 0.0f};
    const ws::math::Vec3 z = ws::math::cross(x, y);
    CHECK(z.z == doctest::Approx(1.0f));
    CHECK(z.x == doctest::Approx(0.0f));
    CHECK(z.y == doctest::Approx(0.0f));
}

TEST_CASE("mat4 identity transform") {
    const ws::math::Vec4 p{1.0f, 2.0f, 3.0f, 1.0f};
    const ws::math::Mat4 identity{1.0f};
    const ws::math::Vec4 q = identity * p;
    CHECK(q.x == doctest::Approx(1.0f));
    CHECK(q.y == doctest::Approx(2.0f));
    CHECK(q.z == doctest::Approx(3.0f));
    CHECK(q.w == doctest::Approx(1.0f));
}

TEST_CASE("double precision aliases exist") {
    const ws::math::DVec3 v{1.5, 2.5, -0.5};
    CHECK(ws::math::length(v) == doctest::Approx(2.95803989154).epsilon(1e-10));
}

TEST_CASE("glm types and Eigen-backed Matrix coexist in one TU") {
    // Compiles = passes. The two backends must never collide.
    const ws::math::Matrix<double> m = {{1.0, 0.0},
                                        {0.0, 1.0}};
    const ws::math::Vec3 v = ws::math::normalize(ws::math::Vec3{1.0f, 1.0f, 0.0f});
    CHECK(m(0, 0) == 1.0);
    CHECK(ws::math::length(v) == doctest::Approx(1.0f).epsilon(1e-6f));
}
