#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/error.hpp>

// The elaborator's public front end (DESIGN.md §2.1 rev): the netlist
// parser is one client of the elaborator, not the front door - this
// builder is the other. It assembles the same card stream the parser
// produces and hands it to the same elaboration path, so a code-built
// circuit and its written netlist are interchangeable by construction
// (the equivalence is pinned by tests):
//
//   using namespace ws::spice::literals;
//   ws::spice::CircuitBuilder c;
//   auto in = c.node("in"), out = c.node("out");
//   auto R1 = c.resistor("R1", in, out, 10_k);
//   c.capacitor("C1", out, ws::spice::gnd, 100_n);
//   c.vsource("V1", in, ws::spice::gnd, Pulse{.v2 = 5, .tr = 1_n, .pw = 1_ms});
//   auto circuit = c.finish().value();
//
// Handles are values: NodeHandle names a node, DeviceHandle names a
// device, and its `.value` / `.param("dc")` are the parameter handles a
// Simulator::set() consumes. Parameter changes through handles can never
// alter the stamp pattern (§3.5 rev), so they never re-analyze.

namespace ws::spice {

// A node reference: opaque (the elaborator owns id assignment), just a
// name. "0"/"gnd" spellings map to ground.
struct NodeHandle {
    std::string name;
};

// Ground, the node every spelling of "0"/"gnd" maps to.
inline const NodeHandle gnd{"0"};

// A value spelled as an expression or a bare .param name - the rebindable
// form (a .step point or Monte-Carlo draw changes the scope, the next run
// re-evaluates it):
//   c.param("rl", "2k");
//   c.resistor("Rl", out, gnd, ExprVal{"rl"});   // or ExprVal{"rl/2"}
struct ExprVal {
    std::string text;
};

// Any device value: a number or an expression spelling.
using Value = std::variant<double, ExprVal>;

struct ParamHandle {
    std::string device;  // "R1"
    std::string param;   // "value", "dc", "gain", ...
};

struct DeviceHandle {
    std::string name;
    ParamHandle value;  // the primary parameter ("value"/"dc"/"gain")

    // Any named parameter of this device.
    [[nodiscard]] ParamHandle param(std::string_view which) const {
        return ParamHandle{name, std::string(which)};
    }
};

// --- source waveforms (the builder spellings of PULSE/SIN/EXP/PWL) ----------

struct Pulse {
    double v1 = 0.0, v2 = 0.0;
    double td = 0.0, tr = 0.0, tf = 0.0, pw = 0.0, per = 0.0;
};
struct Sin {
    double vo = 0.0, va = 0.0;
    double freq = 0.0, td = 0.0, theta = 0.0, phase = 0.0;  // phase in degrees
};
struct ExpSource {
    double v1 = 0.0, v2 = 0.0;
    double td1 = 0.0, tau1 = 0.0, td2 = 0.0, tau2 = 0.0;
};
struct Pwl {
    std::vector<std::pair<double, double>> points;  // (t, v), increasing t
};

using SourceValue = std::variant<double, Pulse, Sin, ExpSource, Pwl, ExprVal>;

struct AcSpec {
    double magnitude = 1.0;
    double phase_deg = 0.0;
};

// A switch .model the builder registers (the card form is .model SW(...)).
struct SwitchModel {
    double ron = 1.0;
    double roff = 1e12;
    std::optional<double> vt;  // S (voltage-controlled)
    std::optional<double> vh;
    std::optional<double> it;  // W (current-controlled)
    std::optional<double> ih;
};

class CircuitBuilder {
public:
    CircuitBuilder() = default;

    // Nodes resolve on first use and ground spellings map to gnd.
    [[nodiscard]] NodeHandle node(std::string name);

    // A global .param, referenced from any value as a bare name or in
    // {expressions} - the rebind point for .step / Monte-Carlo families.
    void param(std::string name, std::string expression);

    // --- devices (all card-grammar equivalents) ------------------------------

    [[nodiscard]] DeviceHandle resistor(std::string name, NodeHandle a, NodeHandle b,
                                         Value ohms);
    [[nodiscard]] DeviceHandle capacitor(std::string name, NodeHandle a, NodeHandle b,
                                         Value farads,
                                         std::optional<double> ic = std::nullopt);
    [[nodiscard]] DeviceHandle inductor(std::string name, NodeHandle a, NodeHandle b,
                                        Value henries,
                                        std::optional<double> ic = std::nullopt,
                                        std::optional<double> rser = std::nullopt);
    // K coupling: names two inductors (case-insensitive), |k| < 1.
    void coupling(std::string name, std::string inductor_a, std::string inductor_b,
                  double k);

    [[nodiscard]] DeviceHandle vsource(std::string name, NodeHandle p, NodeHandle n,
                                       SourceValue value,
                                       std::optional<AcSpec> ac = std::nullopt);
    [[nodiscard]] DeviceHandle isource(std::string name, NodeHandle p, NodeHandle n,
                                       SourceValue value,
                                       std::optional<AcSpec> ac = std::nullopt);

    [[nodiscard]] DeviceHandle vcvs(std::string name, NodeHandle p, NodeHandle n,
                                    NodeHandle cp, NodeHandle cn, Value gain);
    [[nodiscard]] DeviceHandle vccs(std::string name, NodeHandle p, NodeHandle n,
                                    NodeHandle cp, NodeHandle cn, Value gm);
    // The controlling source is named (an existing V source).
    [[nodiscard]] DeviceHandle cccs(std::string name, NodeHandle p, NodeHandle n,
                                    std::string controlling_vsource, Value gain);
    [[nodiscard]] DeviceHandle ccvs(std::string name, NodeHandle p, NodeHandle n,
                                    std::string controlling_vsource, Value r);

    // Switches name their .model; register it with switch_model().
    void vswitch(std::string name, NodeHandle p, NodeHandle n, NodeHandle cp,
                 NodeHandle cn, std::string model, bool initial_on = false);
    void iswitch(std::string name, NodeHandle p, NodeHandle n,
                 std::string controlling_vsource, std::string model,
                 bool initial_on = false);
    void switch_model(std::string name, const SwitchModel& m);

    // Elaborates: same path the deck takes (flatten-free - the builder
    // emits top-level cards only - plus the §6.6 graph checks and the
    // §3.5 rev arena freeze). `temp` is the §7.7 elaboration temperature.
    [[nodiscard]] ws::core::Result<Circuit> finish(double temp = kDefaultTemp,
                                                   std::vector<Warning>* warnings = nullptr);

private:
    struct Card {
        std::string name;
        std::vector<std::string> rest;
    };
    std::vector<Card> cards_;
    std::vector<std::pair<std::string, std::string>> params_;  // name -> expression
    std::vector<std::pair<std::string, SwitchModel>> models_;
};

}  // namespace ws::spice
