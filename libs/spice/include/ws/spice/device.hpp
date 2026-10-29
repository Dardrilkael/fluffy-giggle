#pragma once

#include <cstddef>
#include <complex>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>

// The device interface (DESIGN.md §3). Each device is a polymorphic
// instance with staged hooks; the DC/OP stage is load_dc; M2 grows the
// transient companion (load_tran + the begin_step/commit/LTE state
// machine, §6.2/§6.3) and the small-signal stage (load_ac, §7.4). Later
// milestones grow the same class: nonlinear_residual and per-device
// limiting (M3), behavioral AD (M4), noise (M5).
//
// Index conventions (§3.3): a NodeId IS the MNA voltage-unknown index
// (ground is kGround and every stamp touching it is skipped); branch rows
// (V/L/E/H sources) number after the nodes. Devices never see a lookup
// table.

namespace ws::spice {

using NodeId = std::size_t;
using BranchId = std::size_t;

// Ground: '0' and 'gnd' (case-insensitive) both map here; stamps skip it.
inline constexpr NodeId kGround = static_cast<NodeId>(-1);

class DcLoadContext;
class AcLoadContext;
struct ModelDef;
class ParamTable;

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

    // Claims the next branch row (V/L/E/H sources).
    [[nodiscard]] virtual BranchId claim_branch() = 0;
};

// A device under construction still needing a named branch (F/H name a
// controlling V source; resolved in a second pass once every source has
// its branch).
struct ControlRequest {
    class Device* device = nullptr;  // F/H instance, filled by its factory
    Token control;                   // the V source token it names
};

// K coupling (§6.2): two inductor names + k, resolved after every device
// exists (pairwise only in M2 - the inductor rejects a second partner;
// multi-winding transformers are backlog).
struct CouplingRequest {
    class Device* device = nullptr;  // the K instance (a no-op stamp itself)
    Token a;                         // first inductor name
    Token b;                         // second inductor name
    double k = 0.0;                  // coupling factor, 0 < |k| < 1
};

// S/W switches name a .model card; resolved against the deck's model table
// after every device exists.
struct ModelRequest {
    class Device* device = nullptr;
    Token name;                      // the .model card name
};

// Everything a device factory may ask of the elaborator (§3.4). Node
// resolution and branch claiming happen inline; control/coupling/model
// references defer to a post-pass because the named device may not exist
// yet (deck order).
struct FactoryServices {
    NodeResolver& nodes;
    const ParamTable& params;
    std::vector<ControlRequest>& controls;
    std::vector<CouplingRequest>& couplings;
    std::vector<ModelRequest>& models;
};

// --- the device ----------------------------------------------------------------

// One transient step's parameters (§6.1/§6.2), handed to load_tran and the
// dynamic-device state machine. `time` is the time at the END of the step
// (the new time point); sources evaluate their waveforms there.
struct TranStep {
    double time = 0.0;
    double dt = 0.0;
    bool be = false;  // this step integrates with backward Euler (§6.1)
};

class Device {
public:
    virtual ~Device() = default;

    // "R1" / "X1:R3" - the flattened display name.
    [[nodiscard]] virtual const std::string& name() const = 0;
    // Uppercased kind letter ('R', 'V', 'E', ...).
    [[nodiscard]] virtual char kind() const = 0;
    // True when this device owns a branch row (V/L/E/H) - branch() is valid.
    [[nodiscard]] virtual bool has_branch() const { return false; }
    [[nodiscard]] virtual BranchId branch() const { return 0; }

    // Terminal node ids (for the topological graph checks); ground
    // terminals included as kGround.
    [[nodiscard]] virtual std::vector<NodeId> terminals() const = 0;

    // Nodes this device connects through a CONDUCTANCE path (the DC-path
    // graph check, §6.6): resistors and switches connect their two
    // terminals. Devices that only source current (I, F/G outputs, E/G
    // control inputs) do not. Voltage-constraint devices (V, E/H outputs)
    // connect through, but as constraints - see constraint_edges().
    [[nodiscard]] virtual std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const {
        return {};
    }

    // Voltage-constraint edges (V sources, inductors - a short at DC - and
    // E/H output pairs): nodes these devices pin relative to each other.
    // Cycles among these are voltage-source/inductor loops (§6.6).
    [[nodiscard]] virtual std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const {
        return {};
    }

    // Stamps the DC/OP system. Called once in pattern mode (values
    // ignored, entries recorded) and then per Newton iteration in numeric
    // mode. A numeric write outside the recorded pattern is a programmer
    // error (WS_CORE_ASSERT inside the MNA layer).
    virtual void load_dc(DcLoadContext& cx) const = 0;

    // Transient companion stamp (§6.2). Default: this device is static,
    // its companion IS its DC stamp (R, E/F/G/H). Dynamic devices (C, L)
    // and time-dependent sources override. Coefficients come from
    // begin_step; positions never change with dt.
    virtual void load_tran(DcLoadContext& cx, const TranStep& step) const {
        (void)step;
        load_dc(cx);
    }

    // Small-signal stamp at cx.omega (§7.4). Default: no AC contribution
    // (an I source without an AC spec is open to the small-signal system;
    // a V source without one still pins v(p)-v(n)=0 through its override).
    virtual void load_ac(AcLoadContext& cx) const { (void)cx; }

    // Independent-source sweep hook (.dc): V/I devices accept a new DC
    // value; everything else refuses (the driver errors on those).
    [[nodiscard]] virtual bool set_sweep_value(double /*value*/) { return false; }

    // --- dynamic-device state machine (§3.5, §6.2/§6.3) ----------------------

    // True for devices with per-step companion state (C, L). The driver
    // only runs begin_step/LTE/commit on these.
    [[nodiscard]] virtual bool is_dynamic() const { return false; }

    // Computes this step's companion coefficients from the stored state
    // (v/i/flux/charge history). Runs once per STEP ATTEMPT - a rejected
    // step simply calls it again with the smaller dt; state is untouched
    // until commit.
    virtual void begin_step(const TranStep& step) { (void)step; }

    // Accepts the step: pulls the new v/i from the solution into the
    // device's own state and appends to its LTE history.
    virtual void commit(const std::vector<double>& x, std::size_t node_count,
                        const TranStep& step) {
        (void)x;
        (void)node_count;
        (void)step;
    }

    // The §6.3 LTE test for this device against the candidate solution.
    // `ratio` (out, may be null) is eps/tolq for the timestep update; a
    // device without enough LTE history accepts with ratio 0.
    [[nodiscard]] virtual bool lte_ok(const TranStep& step,
                                      const std::vector<double>& x,
                                      std::size_t node_count, double reltol,
                                      double chgtol, double trtol,
                                      double* ratio) const {
        (void)step;
        (void)x;
        (void)node_count;
        (void)reltol;
        (void)chgtol;
        (void)trtol;
        if (ratio != nullptr) {
            *ratio = 0.0;
        }
        return true;
    }

    // Seeds the dynamic state at t=0 from the OP solution (or the UIC
    // vector: .ic node values, device ic= overrides, zeros elsewhere).
    virtual void seed_state(const std::vector<double>& x, std::size_t node_count) {
        (void)x;
        (void)node_count;
    }

    // Re-evaluates a solution-dependent device state (a switch control)
    // against the candidate x; true when the state changed and the solve
    // must iterate again. Called by the Newton loop after every solve.
    [[nodiscard]] virtual bool update_state(const std::vector<double>& x,
                                            std::size_t node_count) {
        (void)x;
        (void)node_count;
        return false;
    }

    // The next source-schedule event strictly after `t` (§6.4), or +inf
    // when the device publishes none. Valid for waveform sources after
    // finalize(tstep, tstop).
    [[nodiscard]] virtual double next_event(double t) const {
        (void)t;
        return std::numeric_limits<double>::infinity();
    }

    // True when this source carries an `AC mag [phase]` spec (§7.4's
    // no-AC-stimulus check).
    [[nodiscard]] virtual bool has_ac() const { return false; }

    // Resolves the waveform defaults that depend on the .tran card itself
    // (TSTEP/TSTOP - see src/waveform.hpp); the .tran driver calls it on
    // every source before marching.
    virtual void finalize_waveform(double /*tstep*/, double /*tstop*/) {}

    // --- K coupling protocol (src-internal; resolved by the elaborator) -----

    // Offers this device a mutual-inductance partner; false when this is
    // not an inductor or it is already coupled (multi-winding is backlog).
    [[nodiscard]] virtual bool couple_to(Device& partner, double k) {
        (void)partner;
        (void)k;
        return false;
    }
    [[nodiscard]] virtual double inductance() const { return 0.0; }
    [[nodiscard]] virtual double dyn_i_prev() const { return 0.0; }  // L: i_{n-1}
    [[nodiscard]] virtual double dyn_v_prev() const { return 0.0; }  // C/L: v_{n-1}

    // Applies a resolved .model card (S/W switches; R models land M5).
    [[nodiscard]] virtual ws::core::Status apply_model(const ModelDef& model);

    // ic= initial conditions (§6.5): C carries volts, L carries amps.
    [[nodiscard]] virtual bool has_ic() const { return false; }
    [[nodiscard]] virtual double ic_value() const { return 0.0; }

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

// The small-signal twin for .ac (§7.4): same shape, complex values.
// `omega` is the angular frequency being stamped; the device
// linearization is the one frozen at the OP.
class AcLoadContext {
public:
    virtual ~AcLoadContext() = default;

    virtual void add_entry(std::size_t row, std::size_t col, std::complex<double> v) = 0;
    virtual void add_rhs(std::size_t row, std::complex<double> v) = 0;
    [[nodiscard]] virtual std::size_t branch_row(BranchId br) const = 0;

    double omega = 0.0;

    void add_conductance(NodeId a, NodeId b, std::complex<double> g) {
        add_entry(a, a, g);
        add_entry(b, b, g);
        add_entry(a, b, -g);
        add_entry(b, a, -g);
    }
    void add_current(NodeId a, NodeId b, std::complex<double> i) {
        add_rhs(a, i);
        add_rhs(b, -i);
    }
    void stamp_branch_voltage(BranchId br, NodeId p, NodeId n, std::complex<double> v) {
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
    const struct DeviceCard& card, FactoryServices& services);

}  // namespace ws::spice
