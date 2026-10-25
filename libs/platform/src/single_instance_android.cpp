// The Android half of the single-instance grab: the OS owns
// single-instancing (the manifest's launchMode), so every process
// IS the primary and there is no lock to hold. The argv parse
// lives in the header, platform-free.

#include <string_view>

#include <ws/platform/single_instance.hpp>

namespace ws::platform {

SingleInstance::SingleInstance(std::string_view /*key*/)
    : primary_{true} {}

SingleInstance::~SingleInstance() = default;

SingleInstance::SingleInstance(SingleInstance&& other) noexcept
    : handle_{other.handle_}, primary_{other.primary_} {
    other.handle_ = nullptr;
}

SingleInstance& SingleInstance::operator=(SingleInstance&& other) noexcept {
    if (this != &other) {
        handle_ = other.handle_;
        primary_ = other.primary_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool SingleInstance::is_primary() const noexcept { return primary_; }

}  // namespace ws::platform
