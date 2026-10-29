#pragma once

// Internal (src-only): the elaboration output. Circuit is immutable
// topology + device instances (DESIGN.md §2.1); analyses mutate only
// per-analysis state (source sweep values ride set_sweep_value).

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/run.hpp>

namespace ws::spice {

// §7.7: the run's temperature, °C. M1 devices carry no temp dependence
// (models with TC land M5), but the value flows to elaboration so a
// `.dc temp` sweep re-elaborates at each point's temperature and future
// temp-aware models see it.
inline constexpr double kDefaultTemp = 27.0;

struct Circuit {
    // Node table: id -> display name; lookup by lowercased key. Ground is
    // never in the table (kGround sentinel).
    std::vector<std::string> node_names;
    std::map<std::string, NodeId> node_ids;

    std::vector<std::unique_ptr<Device>> devices;

    // V/E/H sources with branch rows, in creation order - the default
    // save-list's i(V1) columns (§0.5).
    std::vector<std::size_t> branch_devices;  // indices into devices

    std::size_t branch_count = 0;

    // The temperature this circuit was elaborated at (§7.7).
    double temp = kDefaultTemp;

    [[nodiscard]] std::size_t node_count() const { return node_names.size(); }
    [[nodiscard]] std::size_t dim() const { return node_names.size() + branch_count; }
};

// Applies .options cards (in deck order, later wins) with validation;
// unknown keys already warned at card parse.
[[nodiscard]] ws::core::Status apply_options(const std::vector<OptionsCard>& options,
                                             RunSettings& out);

// Elaborates the AST into a Circuit: models table, subckt flattening,
// node table, the §6.6 graph checks (§8.4). `temp` is the §7.7
// temperature the devices are built at (`.dc temp` re-elaborates per
// point). Warnings append, never fatal.
[[nodiscard]] ws::core::Result<Circuit> elaborate_deck(const DeckAst& ast, double temp,
                                                       std::vector<Warning>& warnings);

}  // namespace ws::spice
