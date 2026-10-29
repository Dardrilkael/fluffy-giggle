#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>

// The device interface (DESIGN.md §3 - the artifact drafted alongside the
// design document). Each device is a polymorphic instance with staged
// hooks; today's surface is the DC/OP stage: factories resolve their own
// nodes and parameters during elaboration, `load_dc` stamps the MNA system
// (or the pattern recorder - the load path IS the pattern, §3.2). Later
// milestones grow the same class: load_ac (M2), nonlinear_residual and
// per-device limiting (M3), behavioral AD (M4), noise (M5).
//
// Index conventions (§3.3): a NodeId IS the MNA voltage-unknown index
// (ground is kGround and every stamp touching it is skipped); branch rows
// (V/E/H sources) number after the nodes. Devices never see a lookup
// table.

namespace ws::spice {

using NodeId = std::size_t;
using BranchId = std::size_t;

// Ground: '0' and 'gnd' (case-insensitive) both map here; stamps skip it.
inline constexpr NodeId kGround = static_cast<NodeId>(-1);

class DcLoadContext;

// --- elaboration-time services (handed to device factories) -------------------

// A value token from a device card: a SPICE number, an '{expression}', or
// a bare .param name (the ngspice form).
[[nodiscard]] ws::core::Result<double> resolve_value_token(const Token& tok,
                                                           const class ParamTable& params);

class NodeResolver {
public:
    virtual ~NodeResolver() = default;

    // Resolves (creating if new) a node by its token; ground spellings map
    // to kGround. Scope-name mapping (X1:7 style) is the caller's - the
    // flattener rewrites tokens before they get here.
    [[nodiscard]] virtual NodeId node(const Token& name) = 0;

    // Claims the next branch row (V/E/H sources).
    [[nodiscard]] virtual BranchId claim_branch() = 0;
};

// A device under construction still needing a named branch (F/H name a
// controlling V source; resolved in a second pass once every source has
// its branch).
struct ControlRequest {
    class Device* device = nullptr;  // F/H instance, filled by its factory
    Token control;                   // the V source token it names
};

// --- the device ----------------------------------------------------------------

class Device {
public:
    virtual ~Device() = default;

    // "R1" / "X1:R3" - the flattened display name.
    [[nodiscard]] virtual const std::string& name() const = 0;
    // Uppercased kind letter ('R', 'V', 'E', ...).
    [[nodiscard]] virtual char kind() const = 0;
    // True when this device owns a branch row (V/E/H) - branch() is valid.
    [[nodiscard]] virtual bool has_branch() const { return false; }
    [[nodiscard]] virtual BranchId branch() const { return 0; }

    // Terminal node ids (for the topological graph checks); ground
    // terminals included as kGround.
    [[nodiscard]] virtual std::vector<NodeId> terminals() const = 0;

    // Nodes this device connects through a CONDUCTANCE path (the DC-path
    // graph check, §6.6): resistors connect their two terminals. Devices
    // that only source current (I, F/G outputs, E/G control inputs) do
    // not. Voltage-constraint devices (V, E/H outputs) connect through,
    // but as constraints - see constraint_edges().
    [[nodiscard]] virtual std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const {
        return {};
    }

    // Voltage-constraint edges (V sources; E/H output pairs): nodes these
    // devices pin relative to each other. Cycles among these are
    // voltage-source loops (§6.6).
    [[nodiscard]] virtual std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const {
        return {};
    }

    // Stamps the DC/OP system. Called once in pattern mode (values
    // ignored, entries recorded) and then per Newton iteration in numeric
    // mode. A numeric write outside the recorded pattern is a programmer
    // error (WS_CORE_ASSERT inside the MNA layer).
    virtual void load_dc(DcLoadContext& cx) const = 0;

    // Independent-source sweep hook (.dc): V/I devices accept a new DC
    // value; everything else refuses (the driver errors on those).
    [[nodiscard]] virtual bool set_sweep_value(double /*value*/) { return false; }

    // Fills the controlling branch for F/H (second elaboration pass,
    // after every V source claimed its branch). No-op on others.
    virtual void set_control_branch(BranchId /*branch*/) {}

    // Devices own their dynamic state (§3.5); M1 devices are stateless.
    virtual void reset_state() {}
};

// --- load contexts (§3.3) -------------------------------------------------------

// DC/OP/transient stamping surface. The pattern recorder is a private
// subclass inside the MNA layer, so pattern mode runs this same code path.
class DcLoadContext {
public:
    virtual ~DcLoadContext() = default;

    // Raw primitives. row/col index the MNA system (nodes first, then
    // branches); ground (kGround) entries are skipped. Matrix entries
    // ACCUMULATE (+=), rhs too.
    virtual void add_entry(std::size_t row, std::size_t col, double v) = 0;
    virtual void add_rhs(std::size_t row, double v) = 0;

    // The MNA row of a branch unknown: node_count + branch id (§3.3) -
    // branch rows number AFTER the nodes, and only the system knows how
    // many nodes there are, so devices never compute this themselves.
    [[nodiscard]] virtual std::size_t branch_row(BranchId br) const = 0;

    // True while the pattern dry run records (state-dependent devices
    // stamp the union of their states, §3.2).
    [[nodiscard]] virtual bool pattern_mode() const = 0;

    // --- helpers (inline, non-virtual; §3.3) --------------------------------

    // Conductance g between a and b (ground allowed on either side).
    void add_conductance(NodeId a, NodeId b, double g) {
        add_entry(a, a, g);
        add_entry(b, b, g);
        add_entry(a, b, -g);
        add_entry(b, a, -g);
    }

    // Current i injected INTO a, out of b (a current source arrow a <- b).
    void add_current(NodeId a, NodeId b, double i) {
        add_rhs(a, i);
        add_rhs(b, -i);
    }

    // A voltage-source-style branch row for (p, n) with equation
    // v(p) - v(n) = v: stamps the branch equation and the KCL coupling.
    void stamp_branch_voltage(BranchId br, NodeId p, NodeId n, double v) {
        const std::size_t row = branch_row(br);
        add_entry(row, p, 1.0);
        add_entry(row, n, -1.0);
        add_entry(p, row, 1.0);
        add_entry(n, row, -1.0);
        add_rhs(row, v);
    }
};

// The factory signature: card + services -> device. The factory table
// lives on the elaborator object, never a process-global (§3.4).
using DeviceFactory = ws::core::Result<std::unique_ptr<Device>> (*)(
    const struct DeviceCard& card, NodeResolver& nodes, const class ParamTable& params,
    std::vector<ControlRequest>& controls);

}  // namespace ws::spice
