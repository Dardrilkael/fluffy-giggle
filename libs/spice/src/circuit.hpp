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

    [[nodiscard]] std::size_t node_count() const { return node_names.size(); }
    [[nodiscard]] std::size_t dim() const { return node_names.size() + branch_count; }
};

// Applies .options cards (in deck order, later wins) with validation;
// unknown keys already warned at card parse.
[[nodiscard]] ws::core::Status apply_options(const std::vector<OptionsCard>& options,
                                             RunSettings& out);

// Elaborates the AST into a Circuit: models table, subckt flattening,
// node table, the §6.6 graph checks (§8.4). Warnings append, never fatal.
[[nodiscard]] ws::core::Result<Circuit> elaborate_deck(const DeckAst& ast,
                                                       std::vector<Warning>& warnings);

}  // namespace ws::spice
