#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/run.hpp>

// The elaboration output (DESIGN.md §2.1, revised 2026-10-29): a Circuit
// is IMMUTABLE topology plus immutable device construction data - node
// names, devices, branch rows, the .param scope every parameter
// expression evaluates against, and the flattened parameter/state arena
// layout. Everything a run mutates (parameter values, dynamic state) lives
// in a per-run RunState (state.hpp), which is what makes one Circuit
// shareable across .step points, Monte-Carlo runs and parallel Simulators.
//
// This header is public on purpose: it is the type the CircuitBuilder
// produces (the elaborator's input is a public builder, not just the
// netlist parser) and what Simulator::create consumes. The backend rule
// holds - no ws::math type appears here.

namespace ws::spice {

// §7.7: the run's temperature, °C. M1/M2 devices carry no temp dependence
// (models with TC land M5), but the value flows to elaboration so a
// `.dc temp` / `.step temp` sweep re-elaborates at each point's
// temperature and future temp-aware models see it.
inline constexpr double kDefaultTemp = 27.0;

struct Circuit {
    // Node table: id -> display name; lookup by lowercased key. Ground is
    // never in the table (kGround sentinel).
    std::vector<std::string> node_names;
    std::map<std::string, NodeId> node_ids;

    // Immutable device instances, in (flattened) deck order.
    std::vector<std::unique_ptr<Device>> devices;

    // V/E/H/L sources with branch rows, in creation order - the default
    // save-list's i(V1) columns (§0.5).
    std::vector<std::size_t> branch_devices;  // indices into devices

    std::size_t branch_count = 0;

    // The temperature this circuit was elaborated at (§7.7).
    double temp = kDefaultTemp;

    // The global .param scope device parameter expressions evaluate
    // against (subckt-flattened instances bake their overrides into
    // constant programs at elaboration, so one global table serves all).
    ParamTable params;

    // --- the arena layout (frozen by finalize_circuit) ----------------------
    //
    // Every device's parameters and state are slices of two flat
    // per-run vectors: params[param_span[i].begin .. +count) and
    // state[state_span[i].begin .. +count).
    struct Span {
        std::size_t begin = 0;
        std::size_t count = 0;
    };
    std::size_t param_total = 0;
    std::size_t state_total = 0;
    std::vector<Span> param_span;  // [device index]
    std::vector<Span> state_span;  // [device index]
    std::vector<ParamSpec> param_table;  // flattened param_specs()

    // Devices grouped by kind letter, deck order preserved inside a group
    // - the groundwork for batched (per-type) stamping: the MNA load pass
    // may walk groups so a future type override can stamp its whole group
    // under one dispatch. Iteration is deterministic either way (§10.5).
    std::map<char, std::vector<std::size_t>> by_kind;

    [[nodiscard]] std::size_t node_count() const { return node_names.size(); }
    [[nodiscard]] std::size_t dim() const { return node_names.size() + branch_count; }
};

// Freezes a Circuit's arena layout (param_table from every device's
// param_specs(), param_base/state_base, by_kind). Called by the
// elaborator and the CircuitBuilder; idempotent-ish but meant once, at
// the end of construction. Fails through Status on a malformed table.
[[nodiscard]] ws::core::Status finalize_circuit(Circuit& c);

// Applies .options cards (in deck order, later wins) with validation;
// unknown keys already warned at card parse.
[[nodiscard]] ws::core::Status apply_options(const std::vector<OptionsCard>& options,
                                             RunSettings& out);

// Elaborates the AST into a Circuit: models table, subckt flattening,
// node table, the §6.6 graph checks (§8.4). `temp` is the §7.7
// temperature the devices are built at (`.dc temp` / `.step temp`
// re-elaborate per point). Warnings append, never fatal.
[[nodiscard]] ws::core::Result<Circuit> elaborate_deck(const DeckAst& ast, double temp,
                                                       std::vector<Warning>& warnings);

}  // namespace ws::spice
