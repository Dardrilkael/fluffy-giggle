// No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_math.cpp owns doctest's
// main() for the math test binary.

// Deliberately includes ALL ws::math headers in one translation unit: they
// must coexist cleanly (and none of them may pull a third-party header).
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

TEST_CASE("vecmath and Matrix coexist in one TU") {
    // Compiles = passes. The two headers must never collide.
    const ws::math::Matrix<double> m = {{1.0, 0.0},
                                        {0.0, 1.0}};
    const ws::math::Vec3 v = ws::math::normalize(ws::math::Vec3{1.0f, 1.0f, 0.0f});
    CHECK(m(0, 0) == 1.0);
    CHECK(ws::math::length(v) == doctest::Approx(1.0f).epsilon(1e-6f));
}

TEST_CASE("quat: normalize keeps the rotation, unit length") {
    // A 90-degree turn about Z (fields are x, y, z, w), scaled by 7:
    // normalize must recover the same rotation at unit length (normalize's
    // zero input is undefined, like GLM's - callers guard, see ws::physics).
    const ws::math::Quat seven_times{
        0.0f, 0.0f, 7.0f * 0.70710678118f, 7.0f * 0.70710678118f};
    const ws::math::Quat q = ws::math::normalize(seven_times);

    CHECK(q.x == doctest::Approx(0.0f).epsilon(1e-6f));
    CHECK(q.y == doctest::Approx(0.0f).epsilon(1e-6f));
    CHECK(q.z == doctest::Approx(0.70710678118f).epsilon(1e-6f));
    CHECK(q.w == doctest::Approx(0.70710678118f).epsilon(1e-6f));

    // The rotation itself is unchanged: quarter turn about Z maps +X to +Y.
    const ws::math::Vec3 turned = ws::math::rotate(q, ws::math::Vec3{1.0f, 0.0f, 0.0f});
    CHECK(turned.x == doctest::Approx(0.0f).epsilon(1e-5f));
    CHECK(turned.y == doctest::Approx(1.0f).epsilon(1e-5f));
    CHECK(turned.z == doctest::Approx(0.0f).epsilon(1e-5f));
}

TEST_CASE("vec2 cross: the signed-area scalar") {
    using ws::math::Vec2;
    // The unit basis: cross(x_hat, y_hat) = +1 (the parallelogram's area).
    CHECK(ws::math::cross(Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}) ==
          doctest::Approx(1.0f));
    // Reversed: -1.
    CHECK(ws::math::cross(Vec2{0.0f, 1.0f}, Vec2{1.0f, 0.0f}) ==
          doctest::Approx(-1.0f));
    // Collinear: zero.
    CHECK(ws::math::cross(Vec2{2.0f, 0.0f}, Vec2{5.0f, 0.0f}) ==
          doctest::Approx(0.0f));
    // Scaled: cross(2a, b) = 2 * cross(a, b).
    CHECK(ws::math::cross(Vec2{2.0f, 0.0f}, Vec2{0.0f, 1.0f}) ==
          doctest::Approx(2.0f));
    // A triangle's signed area is half of cross(b-a, c-a): a clockwise
    // (y-down screen) unit triangle has area +0.5.
    const Vec2 a{0.0f, 0.0f};
    const Vec2 b{1.0f, 0.0f};
    const Vec2 c{0.0f, 1.0f};
    const float area2 = ws::math::cross(b - a, c - a);
    CHECK(area2 == doctest::Approx(1.0f));  // twice the triangle's area
}

TEST_CASE("right_normal: the y-down clockwise 90-degree turn") {
    using ws::math::Vec2;
    // East -> South (a +90-degree math turn is clockwise on y-down).
    const Vec2 east{1.0f, 0.0f};
    const Vec2 south = ws::math::right_normal(east);
    CHECK(south.x == doctest::Approx(0.0f));
    CHECK(south.y == doctest::Approx(1.0f));
    // South -> West.
    const Vec2 west = ws::math::right_normal(south);
    CHECK(west.x == doctest::Approx(-1.0f));
    CHECK(west.y == doctest::Approx(0.0f));
    // West -> North.
    const Vec2 north = ws::math::right_normal(west);
    CHECK(north.x == doctest::Approx(0.0f));
    CHECK(north.y == doctest::Approx(-1.0f));
    // North -> East (closes the loop).
    const Vec2 back = ws::math::right_normal(north);
    CHECK(back.x == doctest::Approx(1.0f));
    CHECK(back.y == doctest::Approx(0.0f));
    // Length is preserved (a 90-degree rotation is an isometry).
    CHECK(ws::math::length(ws::math::right_normal(Vec2{3.0f, 4.0f})) ==
          doctest::Approx(5.0f));
    // A zero vector maps to zero.
    const Vec2 z = ws::math::right_normal(Vec2{0.0f, 0.0f});
    CHECK(z.x == doctest::Approx(0.0f));
    CHECK(z.y == doctest::Approx(0.0f));
}
