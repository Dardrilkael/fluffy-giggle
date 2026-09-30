// Standalone fuzz driver for the frame parser - NOT a ctest target (it
// runs long by design). Usage:
//   fuzz_net               seed 0x6E6574 ("net"), 20000 iterations
//   fuzz_net <seed>        seed as a decimal number
//   fuzz_net <seed> <iters>
// Exit code: 0 clean, 1 the harness's failure code, 2 bad arguments.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "frame_fuzz.hpp"

int main(int argc, char** argv) {
  std::uint32_t seed = 0x6E6574u;  // "net"
  std::uint32_t iterations = 20000;
  bool ok = true;
  if (argc > 1) {
    ok = std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), seed).ec ==
         std::errc{};
  }
  if (ok && argc > 2) {
    ok = std::from_chars(argv[2], argv[2] + std::strlen(argv[2]), iterations)
             .ec == std::errc{};
  }
  if (!ok) {
    std::fprintf(stderr, "usage: fuzz_net [seed] [iterations]\n");
    return 2;
  }

  const int failure = ws::net::fuzz::run_frame_fuzz(seed, iterations);
  if (failure == 0) {
    std::printf("fuzz_frame: %u iterations (seed %u): clean\n", iterations,
                seed);
    return 0;
  }
  std::printf("fuzz_frame: FAILED (code %d, seed %u, %u iterations)\n", failure,
              seed, iterations);
  return 1;
}
