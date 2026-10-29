#pragma once

#include <cstddef>
#include <complex>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/expr.hpp>
#include <ws/spice/parse.hpp>

// The device interface (DESIGN.md §3, revised 2026-10-29). A Circuit holds
// IMMUTABLE devices: everything a run can change lives outside it, in two
// per-run arenas (state.hpp):
//
//   * PARAMETERS (§3.5a) - the numeric values that feed stamps: a resistor's
//     ohms, a source's DC value, a gain. Each slot carries the expression
//     it is computed from, evaluated against the run's .param scope. A
//     parameter write can NEVER change the stamp pattern (positions depend
//     only on topology), so reuse of the symbolic analysis after a value
//     change is guaranteed by construction - the §7.6 pattern-hash check
//     is retired in favor of that argument.
//   * STATE (§3.5b) - the dynamic evolution a run owns: companion
//     coefficients, charge/flux histories, switch positions, source
//     overrides. Zeroed (re-seeded) when a run starts; copyable for
//     snapshot/restore; never read by another run.
//
// Staging is unchanged from the original design: load_dc / load_tran /
// load_ac stamp the MNA system; the load context carries THIS device's
// parameter and state slices, re-pointed by the system between devices.
// The dynamic hooks (begin_step / lte_ok / commit / seed_state /
// update_state) receive the device's state slice explicitly.
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
class Circuit;
class RunState;

// --- elaboration-time services (handed to device factories) -------------------

// A value token from a device card: a SPICE number, an '{expression}', or
// a bare .param name (the ngspice form). Resolved to its double for
// validation, or captured as a program for the parameter arena.
[[nodiscard]] ws::core::Result<double> resolve_value_token(const Token& tok,
                                                           const class ParamTable& params);
// The same token captured as a parameter source: a constant program, the
// parsed expression, or a lone variable reference. Evaluated per run
// against the (possibly .step-overridden, possibly Monte-Carlo-seeded)
// scope - this is what makes value sweeps and tolerances re-elaboration-
// free.
[[nodiscard]] ws::core::Result<ExprProgram> resolve_value_program(const Token& tok,
                                                                  const ParamTable& params);

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
// (the new time point); sources evaluate their waveforms there. `tstep` /
// `tstop` are the .tran card's own values - waveform parameters that
// ngspice defaults from them (PULSE TR/TF/PW/PER, SIN FREQ, EXP TAUs)
// resolve against these at evaluation time, so a Circuit's waveform data
// stays immutable across runs with different .tran cards.
struct TranStep {
    double time = 0.0;
    double dt = 0.0;
    bool be = false;  // this step integrates with backward Euler (§6.1)
    double tstep = 0.0;
    double tstop = 0.0;
};

// A per-device parameter descriptor (§3.5a): the slot's name ("value",
// "dc", ...) and the expression it is computed from. Circuit finalization
// flattens these into one table; RunState::bind evaluates them.
struct ParamSpec {
    std::string name;
    ExprProgram source;
};

class Device {
public:
    virtual ~Device() = default;

    // "R1" / "X1:R3" - the flattened display name.
    [[nodiscard]] virtual const std::string& name() const = 0;
    // Uppercased kind letter ('R', 'V', 'E', ...).
    [[nodiscard]] virtual char kind() const = 0;
    // Construction provenance (the card's location; the builder fills a
    // synthetic one) - check_params diagnostics render from it.
    SourceLoc loc;
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

    // --- the parameter model (§3.5a) ------------------------------------------
    //
    // param_specs() is called ONCE, at Circuit finalization; the returned
    // descriptors (name + expression per slot) are frozen into the
    // Circuit's flat table. Values live in each RunState.
    [[nodiscard]] virtual std::vector<ParamSpec> param_specs() const { return {}; }

    // Validates bound values (positive, finite, ...). Called by
    // RunState::bind for every rebind - a Monte-Carlo draw is subject to
    // the same rules as a hand-written card value.
    [[nodiscard]] virtual ws::core::Status check_params(const double* /*p*/) const {
        return {};
    }

    // --- the state model (§3.5b) -----------------------------------------------

    // Fixed double slots this device needs in the run's state arena.
    // Variable-size history is expressed as a fixed cap (the LTE window is
    // 3 (t, q) pairs), so a whole run's state is one flat vector<double>
    // - trivially copyable for snapshot/restore, zeroable for reset.
    [[nodiscard]] virtual std::size_t state_slots() const { return 0; }

    // Seeds the state slice from construction-time data (a switch's ic=
    // position). RunState::bind calls it after zeroing; the transient's
    // t=0 OP then overwrites it through seed_state().
    virtual void init_state(double* st) const {
        for (std::size_t i = 0; i < state_slots(); ++i) {
            st[i] = 0.0;
        }
    }

    // --- stamping ---------------------------------------------------------------
    //
    // The context's par()/st() access this device's parameter and state
    // slices (re-pointed by the system between devices). Pattern mode
    // points them at zeroed scratch: stamp POSITIONS must never depend on
    // values (§3.2), only coefficients do.

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

    // --- dynamic-device state machine (§3.5b, §6.2/§6.3) ----------------------

    // True for devices with per-step companion state (C, L). The driver
    // only runs begin_step/LTE/commit on these.
    [[nodiscard]] virtual bool is_dynamic() const { return false; }

    // Folds this device's bound parameters (and any cross-device data it
    // needs - a K-coupled inductor folds its partner's L) into its state
    // slice. The driver calls it once per step attempt, before
    // begin_step: parameters never change mid-solve, so the fold is safe
    // to keep out of the Newton loop.
    virtual void fold_state(double* /*st*/, const class RunState& /*rs*/,
                            const class Circuit& /*c*/) const {}

    // Resolves cross-device links once the arena layout exists (called
    // by finalize_circuit with the device's own index). K-coupled
    // inductors record their partner's parameter slot here so a rebind
    // of either L re-computes the mutual M exactly.
    [[nodiscard]] virtual ws::core::Status link_dynamic(const class Circuit& /*c*/,
                                                        std::size_t /*self*/) {
        return {};
    }

    // Computes this step's companion coefficients from the stored state
    // (v/i/flux/charge history). Runs once per STEP ATTEMPT - a rejected
    // step simply calls it again with the smaller dt; state is untouched
    // until commit.
    virtual void begin_step(const TranStep& step, double* st) { (void)step; (void)st; }

    // Accepts the step: pulls the new v/i from the solution into the
    // device's state slice and appends to its LTE history.
    virtual void commit(const std::vector<double>& x, std::size_t node_count,
                        const TranStep& step, double* st) {
        (void)x;
        (void)node_count;
        (void)step;
        (void)st;
    }

    // The §6.3 LTE test for this device against the candidate solution.
    // `ratio` (out, may be null) is eps/tolq for the timestep update; a
    // device without enough LTE history accepts with ratio 0.
    [[nodiscard]] virtual bool lte_ok(const TranStep& step,
                                      const std::vector<double>& x,
                                      std::size_t node_count, double reltol,
                                      double chgtol, double trtol,
                                      const double* st, double* ratio) const {
        (void)step;
        (void)x;
        (void)node_count;
        (void)reltol;
        (void)chgtol;
        (void)trtol;
        (void)st;
        if (ratio != nullptr) {
            *ratio = 0.0;
        }
        return true;
    }

    // Seeds the dynamic state at t=0 from the OP solution (or the UIC
    // vector: .ic node values, device ic= overrides, zeros elsewhere).
    virtual void seed_state(const std::vector<double>& x, std::size_t node_count,
                            double* st) {
        (void)x;
        (void)node_count;
        (void)st;
    }

    // Re-evaluates a solution-dependent device state (a switch control)
    // against the candidate x; true when the state changed and the solve
    // must iterate again. Called by the Newton loop after every solve.
    [[nodiscard]] virtual bool update_state(const std::vector<double>& x,
                                            std::size_t node_count, double* st) {
        (void)x;
        (void)node_count;
        (void)st;
        return false;
    }

    // The next source-schedule event strictly after `t` (§6.4), or +inf
    // when the device publishes none. tstep/tstop resolve the deferred
    // waveform defaults (see TranStep).
    [[nodiscard]] virtual double next_event(double t, double tstep,
                                             double tstop) const {
        (void)t;
        (void)tstep;
        (void)tstop;
        return std::numeric_limits<double>::infinity();
    }

    // True when this source carries an `AC mag [phase]` spec (§7.4's
    // no-AC-stimulus check).
    [[nodiscard]] virtual bool has_ac() const { return false; }

    // --- K coupling protocol (src-internal; resolved by the elaborator) -----

    // Offers this device a mutual-inductance partner; false when this is
    // not an inductor or it is already coupled (multi-winding is backlog).
    [[nodiscard]] virtual bool couple_to(Device& partner, double k) {
        (void)partner;
        (void)k;
        return false;
    }
    [[nodiscard]] virtual double inductance() const { return 0.0; }

    // Applies a resolved .model card (S/W switches; R models land M5).
    [[nodiscard]] virtual ws::core::Status apply_model(const ModelDef& model);

    // ic= initial conditions (§6.5): C carries volts, L carries amps.
    [[nodiscard]] virtual bool has_ic() const { return false; }
    [[nodiscard]] virtual double ic_value() const { return 0.0; }

    // Fills the controlling branch for F/H (second elaboration pass,
    // after every V source claimed its branch). No-op on others.
    virtual void set_control_branch(BranchId /*branch*/) {}

    // --- netlist round-trip (§10.2 rev) ---------------------------------------
    //
    // Writes this device as one netlist card. `w` names nodes and branch
    // owners and exposes the CURRENT bound parameter values (the writer
    // snapshots a RunState, so a flattened, fully-numeric deck comes out -
    // exactly what a reference simulator wants to consume). Returns false
    // when the device cannot be written: the writer then fails naming it -
    // a loud gap, never a silently truncated netlist.
    class CardWriter;
    [[nodiscard]] virtual bool write_card(std::string& /*out*/,
                                          const CardWriter& /*w*/) const {
        return false;
    }
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

    // --- this device's slices (set by the system before every call) ------
    //
    // params_ points at param_specs().size() doubles; state_ at
    // state_slots() doubles (zeroed scratch in pattern mode). Loads only
    // READ state - the dynamic hooks own mutation through their own
    // slices. par()/st() are the only sanctioned accessors.
    const double* params_ = nullptr;
    const double* state_ = nullptr;

    [[nodiscard]] double par(std::size_t i) const { return params_[i]; }
    [[nodiscard]] double st(std::size_t i) const { return state_[i]; }

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

    // This device's parameter slice (state never enters an AC stamp - the
    // linearization is frozen at the OP).
    const double* params_ = nullptr;
    [[nodiscard]] double par(std::size_t i) const { return params_[i]; }

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

// The netlist-writing view a device's write_card() sees (§10.2 rev):
// node names (ground included), the owning device's name for a branch
// row (F/H/W control references), and this device's bound parameter
// values. One concrete implementation lives in src/netlist.cpp over a
// (Circuit, RunState) pair.
class Device::CardWriter {
public:
    virtual ~CardWriter() = default;
    // The node's display name; "0" for ground.
    [[nodiscard]] virtual const std::string& node(NodeId id) const = 0;
    // The name of the device owning branch `br` (a V/E/H/L source).
    [[nodiscard]] virtual const std::string& branch_owner(BranchId br) const = 0;
    // This device's bound parameter values (param_specs() order).
    [[nodiscard]] virtual const double* params(const Device& dev) const = 0;
    // The shortest exactly-round-tripping spelling of a double.
    [[nodiscard]] static std::string num(double v);
};

}  // namespace ws::spice
