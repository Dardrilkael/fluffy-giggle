#pragma once

#include <cstdint>
#include <vector>

#include <ws/circsim/diag.hpp>
#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/nets.hpp>

// ERC (apps/circsim stage 1) over an existing NetBuild (or a schematic,
// building the nets first). Checks:
//
//   floating pin        a pin whose net holds nothing else (no wire,
//                       junction, other pin or label; fixed-net pins are
//                       never floating - the symbol is the connection)
//   one-pin net         a net with exactly one pin and some attachment
//                       (partially wired; the ground net is exempt)
//   empty net           a net of wires/junctions with no pin at all
//   missing ground      nets exist but none is "0"
//   duplicate refs      two exported parts sharing a ref (ci)
//   ref problems        missing ref, wrong SPICE letter prefix, or a ref
//                       that is not a safe identifier
//   missing value/model per the symbol's needs
//   bad param value     a Number param that is not a SPICE number, or a
//                       Choice param outside its list (stage 2)
//   unused param        a params key the symbol neither specs nor emits -
//                       it can never reach the deck (stage 2, warning)
//   unknown symbol      a part whose key the table does not know
//   V/L loops           a cycle among voltage-source/inductor edges over
//                       the net graph (parallel sources included)
//   I cutsets           a current source bridging two components of the
//                       graph with every current source removed
//
// Warnings never block a run on their own; errors do.

namespace ws::circsim {

// `symbols` resolves keys against a .csym library layered over the
// built-ins (null = built-ins alone).
[[nodiscard]] std::vector<Diag> run_erc(const Schematic& s,
                                        const NetBuild& nets,
                                        const SymbolLibrary* symbols = nullptr);

// Convenience: builds the nets, merges the build diagnostics, then ERCs.
[[nodiscard]] std::vector<Diag> run_erc(
    const Schematic& s, const SymbolLibrary* symbols = nullptr);

}  // namespace ws::circsim
