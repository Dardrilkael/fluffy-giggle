#pragma once

// Fixed-size vector / matrix / quaternion math for graphics and geometry,
// backed by GLM (pinned 1.0.1) and re-exported under ws::math so downstream
// code never touches glm:: names directly.
//
// Division of labor inside ws::math:
//   - matrix.hpp / lu.hpp  -> dynamically-sized dense numerics (Eigen-backed)
//   - vecmath.hpp          -> compile-time-sized graphics math (this file)
// Use these types when sizes are known at compile time.

#include <cstddef>

// GLM is a third-party header - suppress anything MSVC might warn about.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <glm/glm.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace ws::math {

// Float vectors
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

// Double vectors
using DVec2 = glm::dvec2;
using DVec3 = glm::dvec3;
using DVec4 = glm::dvec4;

// Integer vectors
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;

// Matrices (column-major, like OpenGL/GLM conventions)
using Mat2 = glm::mat2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using DMat3 = glm::dmat3;
using DMat4 = glm::dmat4;

using Quat = glm::quat;

// Common operations, re-exported so callers stay inside ws::math.
using glm::cross;
using glm::distance;
using glm::dot;
using glm::length;
using glm::mix;
using glm::normalize;

// Angle helpers (radians).
using glm::radians;
using glm::degrees;

}  // namespace ws::math
