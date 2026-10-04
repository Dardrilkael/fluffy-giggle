// The doctest main lives HERE (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN) -
// test_world_2d.cpp and test_world_3d.cpp include doctest without it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <set>
#include <string_view>
#include <thread>

#include <ws/core/result.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/joint.hpp>
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/joint.hpp>
#include <ws/physics/common.hpp>
#include <ws/physics/errors.hpp>

TEST_CASE("describe covers every PhysicsError, distinctly, from static storage") {
  using ws::physics::PhysicsError;
  const PhysicsError codes[] = {
      PhysicsError::invalid_argument, PhysicsError::invalid_handle,
      PhysicsError::world_full,       PhysicsError::backend_error,
      PhysicsError::not_supported,    PhysicsError::invalid_state,
  };
  std::set<std::string_view> distinct;
  for (const auto code : codes) {
    const std::string_view text = ws::physics::describe(code);
    CHECK_FALSE(text.empty());
    distinct.insert(text);
  }
  CHECK(distinct.size() == std::size(codes));  // no two codes share a line

  // An out-of-range cast falls back instead of misbehaving.
  CHECK(ws::physics::describe(static_cast<PhysicsError>(999)) ==
        "unknown physics error");
}

TEST_CASE("fail builds category-physics errors through the enum") {
  const auto err = ws::physics::fail(ws::physics::PhysicsError::world_full,
                                     "4096 bodies already");
  const ws::core::Result<int> as_result = err;  // converts into any Result
  REQUIRE_FALSE(as_result.has_value());
  CHECK(as_result.error().code ==
        static_cast<std::int32_t>(ws::physics::PhysicsError::world_full));
  CHECK(std::string_view{as_result.error().category} == "physics");
  CHECK(as_result.error().message == "4096 bodies already");

  // Default message = the static describe() line, so Result paths never
  // need a per-call buffer either.
  const auto bare = ws::physics::fail(ws::physics::PhysicsError::invalid_handle);
  const ws::core::Result<int> bare_result = bare;
  CHECK(bare_result.error().message ==
        ws::physics::describe(ws::physics::PhysicsError::invalid_handle));
}

TEST_CASE("make_physics_error keeps the raw-code form internal but correct") {
  const auto err = ws::physics::make_physics_error(
      ws::physics::PhysicsError::backend_error, "detail");
  CHECK(err.code == static_cast<std::int32_t>(ws::physics::PhysicsError::backend_error));
  CHECK(std::string_view{err.category} == "physics");
}

TEST_CASE("generational ids compare by (index, generation), both dimensions") {
  using ws::physics2d::BodyId;
  using ws::physics2d::JointId;
  using BodyId3 = ws::physics3d::BodyId;
  using JointId3 = ws::physics3d::JointId;

  static_assert(BodyId{1, 2} == BodyId{1, 2});
  static_assert(BodyId{1, 2} != BodyId{1, 3});
  static_assert(BodyId{1, 2} != BodyId{2, 2});
  static_assert(JointId{0, 1} == JointId{0, 1});
  static_assert(JointId{0, 1} != JointId{0, 0});  // the zero handle is never issued
  static_assert(BodyId3{1, 2} == BodyId3{1, 2});
  static_assert(JointId3{0, 1} != JointId3{0, 0});

  CHECK(BodyId{} == BodyId{});  // value semantics, no identity surprises
  CHECK(BodyId3{} == BodyId3{});
}

TEST_CASE("published caps are the documented values") {
  CHECK(ws::physics::kMaxBodiesPerWorld == 65535);
  CHECK(ws::physics::kMaxJointsPerWorld == 16383);
  CHECK(ws::physics::kMaxBodiesPerWorld > ws::physics::kMaxJointsPerWorld);
}

TEST_CASE("default_thread_count follows the documented contract, stably") {
  const auto a = ws::physics::default_thread_count();
  const auto b = ws::physics::default_thread_count();
  CHECK(a == b);  // pure hardware query - no reason to drift

  const unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    CHECK(a == 0);  // unknown hardware: single-threaded is always correct
  } else if (hw == 1) {
    CHECK(a == 1);  // one core: degenerates to one worker
  } else {
    // hardware_concurrency - 1, capped to [1, 4] on Android. The test
    // build runs on desktop, so the Android branch never fires here -
    // the common.cpp cap is compile-time and covered by the android build.
    CHECK(a >= 1);
    CHECK(a <= hw - 1);
  }
}
