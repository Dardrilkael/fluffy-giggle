// The backend factory: core creates backends ONLY through this (see
// core/transport.hpp); this side provides them. The single seam point a
// future WSAPoll/epoll backend re-targets.

#include "uv_loop.hpp"

namespace ws::net::detail {

std::unique_ptr<ILoopBackend> make_loop_backend() {
  return std::make_unique<UvLoopBackend>();
}

}  // namespace ws::net::detail
