#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ws/circsim/diag.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/symbols.hpp>

// Net building (apps/circsim stage 1). Connectivity is the reference graph
// - wire endpoints name pins and junctions - plus exact position
// coincidence among attach points:
//
//   * a wire unions its two endpoints;
//   * pins/junctions sharing a grid position union (pin-to-pin touch, a
//     junction dot on a pin);
//   * a wire vertex (endpoint or waypoint) lying on another wire's
//     polyline unions the two wires (the T-junction rule);
//   * a pin or junction lying anywhere on a wire's polyline unions with
//     that wire;
//   * two wires CROSSING where neither has a vertex never connect;
//   * same-name labels (case-insensitive) union the nets they touch;
//   * power/ground pins carry a fixed net name and union with every other
//     pin of that name, wires or not.
//
// Net names, in priority order: ground ("0" - any gnd pin or label spelled
// "0"/"gnd"), a label's name, a power symbol's fixed name, else an auto
// "N001"... Auto names are assigned in a deterministic net order and never
// collide (case-insensitively) with a label/power/ground spelling.

namespace ws::circsim {

struct Net {
    std::uint32_t id = 0;
    std::string name;  // display spelling: "0", "out", "VCC", "N003"
    bool is_ground = false;

    std::vector<PinRef> pins;               // every part pin on the net
    std::vector<std::uint32_t> wires;       // wire ids
    std::vector<std::uint32_t> junctions;   // junction ids
    std::vector<std::uint32_t> labels;      // label ids naming the net
};

class NetList {
public:
    std::vector<Net> nets;

    // (Re)builds the lookup maps from the per-net member lists; build_nets
    // calls it once, and a later mutation + reindex keeps the answers true.
    void index();

    [[nodiscard]] const Net* net_of_pin(PinRef pin) const;
    [[nodiscard]] const Net* net_of_wire(std::uint32_t wire_id) const;
    [[nodiscard]] const Net* net_of_junction(std::uint32_t junction_id) const;
    // Case-insensitive; the display spelling wins the comparison key.
    [[nodiscard]] const Net* by_name(std::string_view name) const;

    // The ground net ("0"), when the schematic has one.
    [[nodiscard]] const Net* ground() const;

private:
    [[nodiscard]] static std::uint64_t pin_key(PinRef r) {
        return (static_cast<std::uint64_t>(r.part) << 32) | r.pin;
    }

    std::unordered_map<std::uint64_t, std::uint32_t> pin_net_;   // -> net id
    std::unordered_map<std::uint32_t, std::uint32_t> wire_net_;
    std::unordered_map<std::uint32_t, std::uint32_t> junction_net_;
    std::unordered_map<std::string, std::uint32_t> name_net_;  // lowercased

    [[nodiscard]] const Net* by_id(std::uint32_t id) const;
};

struct NetBuild {
    NetList nets;
    std::vector<Diag> diags;
};

// Builds the nets. `find` resolves symbol keys (the built-in table by
// default; stage 2 passes the JSON library's lookup). Unknown symbols are
// errors in `diags` and their parts join no net; wires referencing their
// pins are dropped with a warning.
[[nodiscard]] NetBuild build_nets(
    const Schematic& s,
    const SymbolDef* (*find)(std::string_view) = find_symbol);

}  // namespace ws::circsim
