// The Windows half of the single-instance grab: a named mutex
// under Local\ - per-login, not machine-global, so a second login
// starts its own instance. bInitialOwner grabs at once;
// ERROR_ALREADY_EXISTS is the second-instance answer. A failed
// create claims primary anyway - possibly two instances beats
// certainly none.

#include <string>
#include <string_view>

#include <ws/core/assert.hpp>
#include <ws/platform/single_instance.hpp>

#include "win_utf8.hpp"

namespace ws::platform {

SingleInstance::SingleInstance(std::string_view key) {
    // a caller contract, debug-caught: an empty key grabs nothing
    WS_CORE_ASSERT(!key.empty());
    const std::wstring name = L"Local\\" + from_utf8(key);
    // bInitialOwner grabs now, so a second instance never owns the
    // primary's lock - it only observes it.
    handle_ = CreateMutexW(nullptr, TRUE, name.c_str());
    if (handle_ == nullptr) {
        primary_ = true;  // two beats zero
        return;
    }
    primary_ = GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

SingleInstance::SingleInstance(SingleInstance&& other) noexcept
    : handle_{other.handle_}, primary_{other.primary_} {
    other.handle_ = nullptr;
}

SingleInstance& SingleInstance::operator=(SingleInstance&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = other.handle_;
        primary_ = other.primary_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool SingleInstance::is_primary() const noexcept { return primary_; }

}  // namespace ws::platform
