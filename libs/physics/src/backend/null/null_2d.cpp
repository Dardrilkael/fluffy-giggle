// The 2D null backend (internal). Accepts everything, simulates nothing:
// it exists so the lib machinery (handle tables, accumulator, event
// buffer, bulk paths) builds and is tested standalone before the real
// engine lands - and as the seam's proof that World2D never names a
// backend type. Deleted when the Box2D backend arrives (src/backend/box2d).

#include <memory>

#include "core/backend2d.hpp"

namespace ws::physics::detail {
namespace {

class NullWorld2D final : public IWorld2DBackend {
 public:
  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t /*slot*/, const physics2d::BodyDesc& /*desc*/) override {
    return {};
  }
  void destroy_body(std::uint32_t /*slot*/) override {}
  void set_transform(std::uint32_t /*slot*/,
                     const physics2d::Transform& /*t*/) override {}
  void set_velocity(std::uint32_t /*slot*/, ws::math::Vec2 /*linear*/,
                    float /*angular*/) override {}
  void apply_linear_impulse(std::uint32_t /*slot*/,
                            ws::math::Vec2 /*impulse*/) override {}
  void apply_angular_impulse(std::uint32_t /*slot*/,
                            float /*impulse*/) override {}
  void apply_force(std::uint32_t /*slot*/, ws::math::Vec2 /*force*/) override {}
  void apply_torque(std::uint32_t /*slot*/, float /*torque*/) override {}
  [[nodiscard]] BackendVelocity2D get_velocity(
      std::uint32_t /*slot*/) const override {
    return {};
  }

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t /*slot*/, const BackendJoint2D& /*joint*/) override {
    return {};
  }
  void destroy_joint(std::uint32_t /*slot*/) override {}

  void step(float /*dt*/, IEventCollector2D& /*collector*/) override {}
  void copy_transforms(
      std::span<physics2d::Transform> /*out*/) const override {}

  [[nodiscard]] std::optional<BackendRayHit2D> raycast(
      ws::math::Vec2 /*origin*/, ws::math::Vec2 /*direction*/,
      float /*max_distance*/) const override {
    return std::nullopt;
  }
  void overlap_circle(ws::math::Vec2 /*center*/, float /*radius*/,
                      std::vector<std::uint32_t>& out) const override {
    out.clear();
  }
  void overlap_aabb(const physics2d::Aabb& /*box*/,
                    std::vector<std::uint32_t>& out) const override {
    out.clear();
  }
};

}  // namespace

std::unique_ptr<IWorld2DBackend> make_null_world_2d(const BackendDesc2D&) {
  return std::make_unique<NullWorld2D>();
}

}  // namespace ws::physics::detail
