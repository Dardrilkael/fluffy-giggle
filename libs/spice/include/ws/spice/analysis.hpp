#pragma once

#include <optional>
#include <string>
#include <variant>

// Analyses as aggregate structs (DESIGN.md §7 rev): what the Simulator
// runs. The netlist card forms (.op/.dc/.tran/.ac) parse INTO these; the
// builder API constructs them directly; both front ends share the same
// drivers. Designated initializers work:
//   sim.run(Tran{.tstop = 5_ms});
//   sim.run(Ac{.sweep = Ac::Dec, .n = 10, .fstop = 100_k});

namespace ws::spice {

// §7.2: one operating point.
struct OpAnalysis {};

// §7.3: one sweep leg (a source, a range, a step).
struct DcLeg {
    std::string source;  // "V1", "I2", "temp"
    double start = 0.0;
    double stop = 0.0;
    double step = 0.0;
};

// §7.3: sweep a V/I source value (or "temp", deck front end only), with
// an optional nested outer sweep (two levels, like the .dc card).
struct DcAnalysis {
    std::string source;                       // "V1", "I2", "temp"
    double start = 0.0;
    double stop = 0.0;
    double step = 0.0;
    std::optional<DcLeg> outer;                // optional nested sweep
};

// §6: the transient. `tstep` 0 means "tstop/100" (the card form always
// spells it; the builder default keeps Tran{.tstop = ...} tidy).
struct TranAnalysis {
    double tstep = 0.0;
    double tstop = 0.0;
    std::optional<double> tstart;
    std::optional<double> tmax;
    bool uic = false;  // §6.5: no t=0 OP; .ic values are the state
};

// §7.4: the small-signal sweep.
struct AcAnalysis {
    enum class Sweep { Dec, Oct, Lin };
    Sweep sweep = Sweep::Dec;
    int n = 10;
    double fstart = 1.0;
    double fstop = 1.0e6;
};

// One analysis, either front end's spelling.
using Analysis = std::variant<OpAnalysis, DcAnalysis, TranAnalysis, AcAnalysis>;

}  // namespace ws::spice
