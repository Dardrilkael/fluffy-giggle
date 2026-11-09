#pragma once

// Physical constants (DESIGN.md §7.5/§7.7): one header, CODATA 2018/SI-2019
// values, shared by every temp/noise computation in ws::spice. The repo's
// legacy thermal voltage kVtPerK (8.617333262145e-5, the same CODATA set)
// is defined here and the device TUs keep using it.
//
// ngspice 44 carries CODATA 2014 (q = 1.6021766208e-19,
// k = 1.38064852e-23); the relative deltas are ~3e-9 - five significant
// figures of ref-harness agreement hold under either set. The §10.1
// analytic rows compute with THESE values.

#include <cmath>

namespace ws::spice::phys {

// Elementary charge (C, exact per SI-2019).
inline constexpr double kQ = 1.602176634e-19;
// k/q (V/K) - the thermal-voltage slope, matching the historical
// kVtPerK constant in devices_m3/devices_m4.
inline constexpr double kKoverQ = 8.617333262145e-5;
// Boltzmann constant (J/K): k = q * (k/q), the exact SI-2019 value
// 1.380649e-23 to double precision.
inline constexpr double kBoltz = kQ * kKoverQ;
// 0 °C in K.
inline constexpr double kCtoK = 273.15;
// The reference temperature: 27 °C in K (SPICE's REFTEMP).
inline constexpr double kRefTempK = 27.0 + kCtoK;
// SPICE's ln(1e-38) floors for noise densities (noisedef.h N_MINLOG) and
// the |H|^2 gain floor (N_MINGAIN).
inline constexpr double kNoiseMinLog = 1e-38;
inline constexpr double kNoiseMinGain = 1e-20;
// The Nintegrate thresholds (noisedef.h): the log-log slope below which a
// segment is treated as flat, and the |slope+1| below which the
// logarithmic integral form is used.
inline constexpr double kIntFlatThresh = 1e-10;
inline constexpr double kIntUseLog = 1e-10;

// Thermal voltage at temperature `temp_k` (K).
[[nodiscard]] inline double vt(double temp_k) { return temp_k * kKoverQ; }

// Celsius -> Kelvin.
[[nodiscard]] inline double c_to_k(double celsius) { return celsius + kCtoK; }

// The SPICE3 variable bandgap used by the junction-potential and
// Vth(T) machinery: Eg(T) = 1.16 - 7.02e-4*T^2/(T+1108) (diotemp.c,
// mos1temp.c, bjttemp.c - the tlev=0 path).
[[nodiscard]] inline double egfet(double temp_k) {
    return 1.16 - (7.02e-4 * temp_k * temp_k) / (temp_k + 1108.0);
}

// §7.7: the R/C first+second order temperature scaling (restemp.c,
// captemp.c): X(T) = X·(1 + TC1·dt + TC2·dt²), dt = T − Tnom in K.
// TC=0 (the default) returns exactly 1.0 so the stamp is bit-identical.
[[nodiscard]] inline double rc_temp_factor(double temp_k, double tnom_k, double tc1,
                                           double tc2) {
    if (tc1 == 0.0 && tc2 == 0.0) {
        return 1.0;
    }
    const double dt = temp_k - tnom_k;
    return 1.0 + tc1 * dt + tc2 * dt * dt;
}

}  // namespace ws::spice::phys
