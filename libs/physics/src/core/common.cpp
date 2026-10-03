#include <algorithm>
#include <thread>

#include <ws/physics/common.hpp>

namespace ws::physics {

std::uint32_t default_thread_count() noexcept {
  const unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    return 0;  // unknown hardware: single-threaded is always correct
  }
  if (hw == 1) {
    return 1;  // one core: still one worker (the pool degenerates cleanly)
  }
  const std::uint32_t workers = hw - 1;
#if defined(__ANDROID__)
  // big.LITTLE: oversubscribing the little cluster costs more than it
  // wins - cap the pool at 4 workers, the size of one big cluster.
  return std::min(workers, 4u);
#else
  return workers;
#endif
}

}  // namespace ws::physics
