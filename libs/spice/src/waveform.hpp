#pragma once

// Source waveforms (DESIGN.md §6.2/§6.4, M2): PULSE, SIN, EXP, PWL with
// ngspice semantics. Params resolve against the .param scope at parse
// time; the few whose ngspice defaults depend on the .tran card itself
// (TR/TF/PW/PER -> TSTEP/TSTOP, SIN FREQ -> 1/TSTOP, EXP TAUs/TD2 ->
// TSTEP) stay defaulted until finalize(tstep, tstop), which the .tran
// driver calls before marching. value(0) is valid pre-finalize (none of
// the t=0 forms depend on the deferred defaults), so a waveform source's
// DC value for a plain .op is waveform(0) - ngspice's behavior.

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/parse.hpp>

namespace ws::spice {

class Waveform {
public:
    enum class Kind { Pulse, Sin, Exp, Pwl };

    Kind kind = Kind::Pulse;
    // Pulse: [0]=V1 [1]=V2 [2]=TD [3]=TR [4]=TF [5]=PW [6]=PER
    // Sin:   [0]=VO [1]=VA [2]=FREQ [3]=TD [4]=THETA [5]=PHASE(deg)
    // Exp:   [0]=V1 [1]=V2 [2]=TD1 [3]=TAU1 [4]=TD2 [5]=TAU2
    double p[7] = {0, 0, 0, 0, 0, 0, 0};
    bool defaulted[7] = {false, false, false, false, false, false, false};

    // Pwl: corners (t, v), strictly increasing t.
    std::vector<std::pair<double, double>> pwl;

    // The waveform value at t (ngspice piece definitions).
    [[nodiscard]] double value(double t) const;

    // The next schedule event strictly after `t` (a PULSE edge, SIN/EXP
    // start, PWL corner), or +inf. Valid after finalize().
    [[nodiscard]] double next_event(double t) const;

    // Resolves the tstep/tstop-dependent defaults (see the header comment).
    void finalize(double tstep, double tstop);

    [[nodiscard]] const char* name() const {
        switch (kind) {
            case Kind::Pulse: return "PULSE";
            case Kind::Sin: return "SIN";
            case Kind::Exp: return "EXP";
            case Kind::Pwl: return "PWL";
        }
        return "?";
    }
};

// Parses a waveform from its card tokens. `tokens` are the raw record
// tokens spanning the waveform - either one ("PULSE(0 1 1u)") or several
// ("PULSE(" "0" "1" "1u" ")") - starting at tokens[0]. Every argument
// resolves through the .param scope (numbers, {expr}, bare names).
[[nodiscard]] ws::core::Result<Waveform> parse_waveform(const std::vector<Token>& tokens,
                                                        const ParamTable& params);

// True when `lower` (an already-lowercased token) starts a waveform:
// "pulse(", "sin(", "exp(", "pwl(".
[[nodiscard]] bool is_waveform_start(const std::string& lower);

}  // namespace ws::spice
