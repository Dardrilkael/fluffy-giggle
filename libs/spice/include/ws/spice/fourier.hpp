#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Fourier analysis as a LIBRARY function (§7.9), the measure:: pattern
// (§0.4): one signal column plus its (time) axis in, the harmonic series
// out. The window is the LAST whole fundamental period of the run - the
// settled waveform, not the startup - and the DFT bins are that window's
// exact harmonics, integrated trapezoidally on the ADAPTIVE grid (the
// same no-resample convention the measure:: library compares with the
// ngspice harness). A non-empty note means "cannot be computed" - never
// a fabricated number; the caller turns it into a .four failure or a
// library-level report.

namespace ws::spice::fourier {

// One harmonic line (n = 1 is the fundamental).
struct Harmonic {
    int n = 0;
    double freq = 0.0;        // n * fundamental, Hz
    double magnitude = 0.0;   // amplitude, the signal's own unit
    double phase_deg = 0.0;   // cos-referenced: x ~ mag*cos(2*pi*f*t + phase)
    double normalized = 0.0;  // magnitude / fundamental * 100
};

struct Series {
    double fundamental = 0.0;         // Hz, as requested
    std::size_t periods = 0;          // whole fundamental periods in the window
    double dc = 0.0;                  // the n = 0 component
    std::vector<Harmonic> harmonics;  // n = 1 .. count
    double thd_percent = 0.0;         // sqrt(sum_{n>=2} mag^2) / mag_1 * 100
    std::string note;                 // non-empty = could not be computed
    [[nodiscard]] bool ok() const { return note.empty(); }
};

// The harmonic series of `sig` over its (time) axis. `fundamental_hz`
// must be finite and > 0, `harmonics` a whole count in [1, 10000] -
// anything else is a note. When the run is shorter than one fundamental
// period the whole run is the window and `periods` reports 0: the bins
// still answer, but they carry window leakage (the caller can say so).
[[nodiscard]] Series series(std::span<const double> axis,
                            std::span<const double> sig,
                            double fundamental_hz, int harmonics);

}  // namespace ws::spice::fourier
