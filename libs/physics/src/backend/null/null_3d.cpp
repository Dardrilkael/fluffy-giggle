// The 3D null backend (internal): the exact mirror of null_2d.cpp - accepts
// everything, simulates nothing, exists until the Jolt backend arrives
// (src/backend/jolt3d). See null_2d.cpp for the rationale.

#include <memory>

#include "core/backend3d.hpp"

namespace ws::physics::detail {
namespace {

class NullWorld3D final : public IWorld3DBackend {
 public:
  [[nodiscard]] ws::core::Status create_body(
      std::uint32_t /*slot*/, const physics3d::BodyDesc& /*desc*/) override {
    return {};
  }
  void destroy_body(std::uint32_t /*slot*/) override {}
  void set_transform(std::uint32_t /*slot*/,
                     const physics3d::Transform& /*t*/) override {}
  void set_velocity(std::uint32_t /*slot*/, ws::math::Vec3 /*linear*/,
                    ws::math::Vec3 /*angular*/) override {}
  void apply_linear_impulse(std::uint32_t /*slot*/,
                            ws::math::Vec3 /*impulse*/) override {}
  void apply_angular_impulse(std::uint32_t /*slot*/,
                            ws::math::Vec3 /*impulse*/) override {}
  void apply_force(std::uint32_t /*slot*/, ws::math::Vec3 /*force*/) override {}
  void apply_torque(std::uint32_t /*slot*/, ws::math::Vec3 /*torque*/) override {}
  [[nodiscard]] BackendVelocity3D get_velocity(
      std::uint32_t /*slot*/) const override {
    return {};
  }

  [[nodiscard]] ws::core::Status create_joint(
      std::uint32_t /*slot*/, const BackendJoint3D& /*joint*/) override {
    return {};
  }
  void destroy_joint(std::uint32_t /*slot*/) override {}

  void step(float /*dt*/, IEventCollector3D& /*collector*/) override {}
  void copy_transforms(
      std::span<physics3d::Transform> /*out*/) const override {}

  [[nodiscard]] std::optional<BackendRayHit3D> raycast(
      ws::math::Vec3 /*origin*/, ws::math::Vec3 /*direction*/,
      float /*max_distance*/) const override {
    return std::nullopt;
  }
  void overlap_sphere(ws::math::Vec3 /*center*/, float /*radius*/,
                      std::vector<std::uint32_t>& out) const override {
    out.clear();
  }
  void overlap_aabb(const physics3d::Aabb& /*box*/,
                    std::vector<std::uint32_t>& out) const override {
    out.clear();
  }
};

}  // namespace

std::unique_ptr<IWorld3DBackend> make_null_world_3d(const BackendDesc3D&) {
  return std::make_unique<NullWorld3D>();
}

}  // namespace ws::physics::detail
