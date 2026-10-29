#pragma once

#include <cstdint>

// Typed unit literals (DESIGN.md §8.2 rev, the builder API's spelling):
// the SPICE scale factors as UDLs, so C++ circuits read like netlists -
//   c.resistor("R1", in, out, 10_k);      // 10 kilo-ohms
//   c.capacitor("C1", out, gnd, 100_n);   // 100 nanofarads
//   sim.run(Tran{.tstop = 5_ms});         // 5 milliseconds
//
// The factors are the SPICE suffix table verbatim (MEG before M, etc. is
// the PARSER's problem; here each suffix is unambiguous). _ms/_us are
// readability aliases for time contexts - a SPICE value is a plain double
// and the suffix only picks the scale.

namespace ws::spice::literals {

constexpr double operator""_T(unsigned long long v) { return static_cast<double>(v) * 1e12; }
constexpr double operator""_G(unsigned long long v) { return static_cast<double>(v) * 1e9; }
constexpr double operator""_meg(unsigned long long v) { return static_cast<double>(v) * 1e6; }
constexpr double operator""_k(unsigned long long v) { return static_cast<double>(v) * 1e3; }
constexpr double operator""_m(unsigned long long v) { return static_cast<double>(v) * 1e-3; }
constexpr double operator""_u(unsigned long long v) { return static_cast<double>(v) * 1e-6; }
constexpr double operator""_n(unsigned long long v) { return static_cast<double>(v) * 1e-9; }
constexpr double operator""_p(unsigned long long v) { return static_cast<double>(v) * 1e-12; }
constexpr double operator""_f(unsigned long long v) { return static_cast<double>(v) * 1e-15; }
// Time-context aliases.
constexpr double operator""_ms(unsigned long long v) { return static_cast<double>(v) * 1e-3; }
constexpr double operator""_us(unsigned long long v) { return static_cast<double>(v) * 1e-6; }

constexpr double operator""_T(long double v) { return static_cast<double>(v) * 1e12; }
constexpr double operator""_G(long double v) { return static_cast<double>(v) * 1e9; }
constexpr double operator""_meg(long double v) { return static_cast<double>(v) * 1e6; }
constexpr double operator""_k(long double v) { return static_cast<double>(v) * 1e3; }
constexpr double operator""_m(long double v) { return static_cast<double>(v) * 1e-3; }
constexpr double operator""_u(long double v) { return static_cast<double>(v) * 1e-6; }
constexpr double operator""_n(long double v) { return static_cast<double>(v) * 1e-9; }
constexpr double operator""_p(long double v) { return static_cast<double>(v) * 1e-12; }
constexpr double operator""_f(long double v) { return static_cast<double>(v) * 1e-15; }
constexpr double operator""_ms(long double v) { return static_cast<double>(v) * 1e-3; }
constexpr double operator""_us(long double v) { return static_cast<double>(v) * 1e-6; }

}  // namespace ws::spice::literals
