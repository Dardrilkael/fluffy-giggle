#pragma once

// Source waveforms (DESIGN.md §6.2/§6.4, M2): PULSE, SIN, EXP, PWL with
// ngspice semantics. Params resolve against the .param scope at parse
// time; the few whose ngspice defaults depend on the .tran card itself
// (TR/TF/PW/PER -> TSTEP/TSTOP, SIN FREQ -> 1/TSTOP, EXP TAUs/TD2 ->
// TSTEP) stay flagged in `defaulted` and resolve INLINE at evaluation
// time through the (tstep, tstop) overloads - the waveform never
// mutates, so a Circuit stays immutable across runs with different .tran
// cards. value(t) (no card context) is only safe where no deferred
// default matters; the t=0 DC value never does (none of the t=0 forms
// depend on them) - that is ngspice's "waveform(0) is the DC value".

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

    // The waveform value at t (ngspice piece definitions) with the
    // tstep/tstop-dependent defaults resolved (see the header comment).
    [[nodiscard]] double value(double t, double tstep, double tstop) const;

    // The raw-parameter value - valid only where no deferred default
    // matters (in practice: the t=0 DC value).
    [[nodiscard]] double value(double t) const;

    // The next schedule event strictly after `t` (a PULSE edge, SIN/EXP
    // start, PWL corner), or +inf, with defaults resolved.
    [[nodiscard]] double next_event(double t, double tstep, double tstop) const;

    [[nodiscard]] const char* name() const {
        switch (kind) {
            case Kind::Pulse: return "PULSE";
            case Kind::Sin: return "SIN";
            case Kind::Exp: return "EXP";
            case Kind::Pwl: return "PWL";
        }
        return "?";
    }

private:
    // Effective parameters: p with every deferred default resolved
    // against the .tran card's tstep/tstop (the old finalize(), made
    // pure - a local array, never a mutation).
    void resolve_into(double out[7], double tstep, double tstop) const;
    [[nodiscard]] double value_with(const double q[7], double t) const;
    [[nodiscard]] double next_event_with(const double q[7], double t) const;
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
