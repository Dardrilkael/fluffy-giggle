#pragma once

#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>

// The lint pass (§9.3 rev): the no-simulate checks behind `spice check` -
// the §6.6 topology findings, unused .model cards, and the SPICE number
// traps that bite every ngspice user (M = milli on a resistor that
// surely meant MEG; F = femto where farads were meant). Findings are
// Warnings (§9.3 shape, never fatal here); hard deck errors (parse,
// elaboration) propagate as errors.

namespace ws::spice {

// Scans the deck's top-level device value tokens for the famous
// scale-suffix traps (§8.2), appending warnings:
//   * a resistor whose value carries the M suffix - 10M is TEN MILLI-OHMS;
//     mega is spelled MEG,
//   * a non-capacitor whose value carries the F suffix - 10F is
//     1e-14 volts, femto, not farads,
//   * a capacitor whose value carries the F suffix - 100F is 100
//     femtofarads, not 100 farads.
void lint_number_traps(const DeckAst& ast, std::vector<Warning>& warnings);

// The full check: number traps + a full elaboration (the §6.6 graph
// findings and unused-model warnings surface from there). Returns ok
// whatever the warning count (warnings never fail anything, §9.3); deck
// errors propagate.
[[nodiscard]] ws::core::Status lint_deck(const DeckAst& ast,
                                         std::vector<Warning>& warnings);

}  // namespace ws::spice
