#pragma once

#include <string>

#include <ws/core/result.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/state.hpp>

// Circuit -> netlist (DESIGN.md §10.2 rev): writes a fully-flattened,
// fully-numeric deck - every subckt expanded, every parameter evaluated
// to its current bound value, every referenced .model emitted. That is
// exactly the artifact a reference simulator (ngspice) consumes, so the
// ref harness can cross-check any code-built OR deck-built circuit
// without a second format. The round-trip is an equivalence test:
// parse(write(c)) must elaborate to the same circuit and solve to the
// same numbers.

namespace ws::spice {

struct NetlistOptions {
    // The first line of a SPICE deck is its title.
    std::string title = "ws-spice netlist";
    // Emit `* temp = 27 C` provenance comment ahead of the devices.
    bool comment_temp = true;
};

// Writes `c` with `rs`'s bound parameter values (a snapshot: Simulator
// set()/set_source() changes appear when the Simulator's own state is
// handed over). Fails naming the first unwritable device instead of
// writing a silently truncated deck.
[[nodiscard]] ws::core::Result<std::string> write_netlist(const Circuit& c,
                                                          const RunState& rs,
                                                          const NetlistOptions& opts = {});

}  // namespace ws::spice
