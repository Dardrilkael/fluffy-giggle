// M4e (DESIGN.md §6.2, §11): the lossless transmission line (T) - the
// last M4 device. Card grammar (ngspice-canonical, the LTspice spellings
// accepted):
//
//   T name in+ in- out+ out- Z0=<val> TD=<val>
//   T name in+ in- out+ out- Z0=<val> F=<val> [NL=<val>]   (TD = NL/F)
//     ... [IC=<v1>[,]i1[,v2[,i2]]]   (comma- or space-separated; UIC)
//
// Method of characteristics. Port k is a Thevenin source
//
//   v_k = Z0*i_k + E_k,   E1(t) = v2(t-TD) + Z0*i2(t-TD),
//                         E2(t) = v1(t-TD) + Z0*i1(t-TD)
//
// - the EMF each port sees is the wave the OTHER port launched one delay
// ago. The two port currents are the device's branch unknowns; the branch
// rows carry the Thevenin equations (exactly ngspice's TRAibr1/TRAibr2
// rows with its internal int1/int2 nodes eliminated), and the branch
// columns enter KCL with the V-source convention (current into the device
// at the + node, out at the - node).
//
// DC/OP is the steady state the delay hides: v1 = v2 and i1 = -i2 - the
// wire pair, stamped over the same two branch rows as an ideal 1:1
// transformer (what the line degenerates to when nothing varies; ngspice
// stamps the same equations plus a (1-gmin) regularization we do not
// need - the graph checks of §6.6 already carry the singular cases).
//
// AC is the lossless two-port: V1 - Z0*I1 = (V2 + Z0*I2)e^{-j*w*TD} and
// its mirror, stamped on the same rows. At omega == 0 those two rows
// coincide, so the second falls back to the DC current equation (i1 +
// i2 = 0); positions are shared with the nonzero-omega form, so the
// pattern is omega-independent (§3.2 - the dry run must see the richest
// shape, which is why MnaSystem::setup stamps the AC pass at a nominal
// omega = 1).
//
// The history (§6.2 "delay ring buffer and interpolated pickup"): a
// fixed-capacity ring of (t, W1, W2) triples, W_k = v_k + Z0*i_k being
// the wave port k launches. Seeded with the t=0 state repeated over
// [-TD, 0] (the line sits at its DC steady state before t=0); pruned to
// the pair bracketing the current pickup; appended per accepted step.
// The pickup is LINEAR interpolation: exact on the piecewise-linear legs
// every real waveform is made of, and no overshoot at corners - ngspice
// runs 3-point Lagrange, which bends across a vertex; a delay line's
// whole job is carrying legs intact, so linear wins (the deviation is
// documented in CLAUDE.md).
//
// Timestep control (§6.2 "delay crossings register breakpoints"): every
// stored sample registers an event at t + TD through next_event, which
// (a) caps dt at TD - the pickup t-TD never extrapolates past the newest
// sample - and (b) lands every delayed kink exactly on a grid point (BE
// restart after it, §6.1), so edges step out of the line as cleanly as
// they stepped in. The line itself publishes NO LTE: it has no
// charge/flux to integrate (§6.3 is about those), its companion is
// exact between delayed vertices, and the curvature the ports see rides
// the LTE of the dynamic devices attached to them.

#include "cards.hpp"

#include "physconst.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

namespace {

Diagnostic dev_diag(ErrorKind kind, std::string code, std::string message,
                    SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

double v_at(const std::vector<double>& x, NodeId n) {
    return (n == kGround) ? 0.0 : x[n];
}

// --- T -----------------------------------------------------------------------

// State layout (state_slots == 6 + 3*kRingCap):
//   [0] Z0 (folded), [1] TD (folded) - begin_step/commit read the slice,
//       which has no parameter access (§3.5b).
//   [2] E1, [3] E2 - this step's Thevenin EMFs (begin_step writes them
//       once per attempt; load_tran stamps them as rhs).
//   [4] ring head - index of the oldest live sample (0..kRingCap-1).
//   [5] ring count - live samples, 0..kRingCap.
//   [6..] the ring: kRingCap triples (t, W1, W2), logical order head,
//       head+1, ... modulo kRingCap.
class TlineDevice final : public Device {
public:
    // One delay window of accepted steps. Overflow is a loud Timestep
    // error through load_error (the only failure channel a load path
    // has), not a silent overwrite: a deck that puts more than this many
    // curvature-carrying steps inside one delay is resolving detail the
    // line cannot transport anyway.
    static constexpr std::size_t kRingCap = 2048;

    static constexpr std::size_t kZ0 = 0, kTd = 1, kE1 = 2, kE2 = 3;
    static constexpr std::size_t kHead = 4, kCount = 5, kRing = 6;

    TlineDevice(std::string display, NodeId n1, NodeId n2, NodeId n3, NodeId n4,
                ExprProgram z0, ExprProgram td, BranchId br1, BranchId br2,
                bool has_ic, double ic_v1, double ic_i1, double ic_v2, double ic_i2,
                SourceLoc sl)
        : name_(std::move(display)), n1_(n1), n2_(n2), n3_(n3), n4_(n4),
          z0_(std::move(z0)), td_(std::move(td)), br1_(br1), br2_(br2),
          has_ic_(has_ic), ic_v1_(ic_v1), ic_i1_(ic_i1), ic_v2_(ic_v2),
          ic_i2_(ic_i2) {
        this->loc = sl;
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'T'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    // The primary branch is the port-1 current (the save-list's i(T1));
    // br2_ is the port-2 current, same stamping shape, no owner-facing
    // name of its own.
    [[nodiscard]] BranchId branch() const override { return br1_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {n1_, n2_, n3_, n4_};
    }
    // At DC the line IS the wire pair (v1 = v2, i1 = -i2), so the graph
    // checks see exactly that: the port + nodes pin together, the port -
    // nodes pin together - parallel lines / V-source loops over the same
    // pairs are the §6.6 constraint findings. In tran the companion is a
    // conductance at each port, but the elaboration checks are DC-
    // structural by design.
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{n1_, n3_}, {n2_, n4_}};
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"z0", z0_}, ParamSpec{"td", td_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[0] > 0.0) || !std::isfinite(p[0])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.t_value",
                "transmission line " + name_ + " needs Z0 > 0", loc));
        }
        if (!(p[1] > 0.0) || !std::isfinite(p[1])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.t_value",
                "transmission line " + name_ + " needs TD > 0 (or F= and NL=)"
                " - a zero-delay line is a wire pair; use a plain connection",
                loc));
        }
        return {};
    }

    // --- stamps ----------------------------------------------------------------

    void load_dc(DcLoadContext& cx) const override {
        // The wire pair over the two branch rows:
        //   row1: v(n1) - v(n2) - v(n3) + v(n4) = 0   (v1 = v2)
        //   row2: i1 + i2 = 0                          (i1 = -i2)
        // plus the KCL coupling both branch columns carry in every mode.
        stamp_kcl(cx);
        const std::size_t r1 = cx.branch_row(br1_);
        const std::size_t r2 = cx.branch_row(br2_);
        cx.add_entry(r1, n1_, 1.0);
        cx.add_entry(r1, n2_, -1.0);
        cx.add_entry(r1, n3_, -1.0);
        cx.add_entry(r1, n4_, 1.0);
        cx.add_entry(r2, r1, 1.0);
        cx.add_entry(r2, r2, 1.0);
    }

    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        // The Thevenin companions:
        //   row1: v(n1) - v(n2) - Z0*i1 = E1
        //   row2: v(n3) - v(n4) - Z0*i2 = E2
        stamp_kcl(cx);
        const std::size_t r1 = cx.branch_row(br1_);
        const std::size_t r2 = cx.branch_row(br2_);
        const double z0 = cx.st(kZ0);
        cx.add_entry(r1, n1_, 1.0);
        cx.add_entry(r1, n2_, -1.0);
        cx.add_entry(r1, r1, -z0);
        cx.add_rhs(r1, cx.st(kE1));
        cx.add_entry(r2, n3_, 1.0);
        cx.add_entry(r2, n4_, -1.0);
        cx.add_entry(r2, r2, -z0);
        cx.add_rhs(r2, cx.st(kE2));
    }

    void load_ac(AcLoadContext& cx) const override {
        stamp_kcl(cx);
        const std::size_t r1 = cx.branch_row(br1_);
        const std::size_t r2 = cx.branch_row(br2_);
        const double z0v = cx.par(0);
        const double td = cx.par(1);
        if (cx.omega > 0.0) {
            // V1 - Z0*I1 = (V2 + Z0*I2) e^{-j*w*TD} and its mirror.
            const std::complex<double> z0{z0v, 0.0};
            const std::complex<double> c =
                std::exp(std::complex<double>{0.0, -cx.omega * td});
            cx.add_entry(r1, n1_, 1.0);
            cx.add_entry(r1, n2_, -1.0);
            cx.add_entry(r1, r1, -z0);
            cx.add_entry(r1, n3_, -c);
            cx.add_entry(r1, n4_, c);
            cx.add_entry(r1, r2, -c * z0);
            cx.add_entry(r2, n3_, 1.0);
            cx.add_entry(r2, n4_, -1.0);
            cx.add_entry(r2, r2, -z0);
            cx.add_entry(r2, n1_, -c);
            cx.add_entry(r2, n2_, c);
            cx.add_entry(r2, r1, -c * z0);
        } else {
            // omega == 0: the two port equations coincide (c = 1), so the
            // second row falls back to the DC current equation. Positions
            // are a subset of the nonzero-omega form's, so the frozen
            // pattern covers both (§3.2).
            cx.add_entry(r1, n1_, 1.0);
            cx.add_entry(r1, n2_, -1.0);
            cx.add_entry(r1, r1, -std::complex<double>{z0v, 0.0});
            cx.add_entry(r1, n3_, -1.0);
            cx.add_entry(r1, n4_, 1.0);
            cx.add_entry(r1, r2, -std::complex<double>{z0v, 0.0});
            cx.add_entry(r2, r1, 1.0);
            cx.add_entry(r2, r2, 1.0);
        }
    }

    // --- dynamic-device state machine (§3.5b/§6.2) -----------------------------

    [[nodiscard]] bool is_dynamic() const override { return true; }
    [[nodiscard]] std::size_t state_slots() const override {
        return kRing + 3 * kRingCap;
    }
    void fold_state(double* st, const RunState& rs, const Circuit& c) const override;
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& c,
                                                std::size_t self) override {
        self_index_ = self;
        (void)c;
        return {};
    }

    void begin_step(const TranStep& step, double* st) override {
        const double p = step.time - st[kTd];
        // The bracketing pair around the pickup p (samples are in
        // ascending time from the head). The first sample at or after p
        // closes the bracket; p is never past the newest sample because
        // next_event caps dt at TD.
        const std::size_t count = ring_count(st);
        double w1_lo = 0.0, w2_lo = 0.0, w1_hi = 0.0, w2_hi = 0.0;
        double t_lo = 0.0;
        bool exact = false;
        std::size_t i = 0;
        for (; i < count; ++i) {
            const double ts = ring_t(st, i);
            if (ts >= p) {
                w1_hi = ring_w1(st, i);
                w2_hi = ring_w2(st, i);
                if (i > 0) {
                    t_lo = ring_t(st, i - 1);
                    w1_lo = ring_w1(st, i - 1);
                    w2_lo = ring_w2(st, i - 1);
                } else {
                    // p at or before the oldest sample: the pre-history is
                    // constant (the seed), so the oldest value IS the
                    // pickup.
                    exact = true;
                }
                break;
            }
        }
        if (i == count) {
            // p at or after the newest sample (dt == TD, or a fresh state
            // no transient has seeded): clamp to the newest value.
            exact = true;
            if (count > 0) {
                w1_hi = ring_w1(st, count - 1);
                w2_hi = ring_w2(st, count - 1);
            }
        }
        if (exact || count == 0 || w1_hi == w1_lo) {
            st[kE1] = w2_hi;
            st[kE2] = w1_hi;
            return;
        }
        const double span = ring_t(st, i) - t_lo;
        const double f = (span > 0.0) ? (p - t_lo) / span : 0.0;
        // E1 comes from port 2's wave, E2 from port 1's (the cross of
        // §6.2: each port sees what the other launched).
        st[kE1] = w2_lo + (w2_hi - w2_lo) * f;
        st[kE2] = w1_lo + (w1_hi - w1_lo) * f;
    }

    void commit(const std::vector<double>& x, std::size_t node_count,
                const TranStep& step, double* st) override {
        const double z0 = st[kZ0];
        const double w1 =
            (v_at(x, n1_) - v_at(x, n2_)) + z0 * x[node_count + br1_];
        const double w2 =
            (v_at(x, n3_) - v_at(x, n4_)) + z0 * x[node_count + br2_];
        prune(st, step.time);
        append(st, step.time, w1, w2);
    }

    void seed_state(const std::vector<double>& x, std::size_t node_count,
                    double* st) override {
        const double z0 = st[kZ0];
        const double td = st[kTd];
        double w1 = 0.0;
        double w2 = 0.0;
        if (has_ic_) {
            // IC= replaces the t=0 waves wholesale (the C/L ic= precedent:
            // honored at seed time; ngspice reads them under UIC only).
            w1 = ic_v1_ + z0 * ic_i1_;
            w2 = ic_v2_ + z0 * ic_i2_;
        } else {
            w1 = (v_at(x, n1_) - v_at(x, n2_)) + z0 * x[node_count + br1_];
            w2 = (v_at(x, n3_) - v_at(x, n4_)) + z0 * x[node_count + br2_];
        }
        st[kHead] = 0.0;
        st[kCount] = 2.0;
        st[kRing + 0] = -td;  // the constant pre-history over [-TD, 0]
        st[kRing + 1] = w1;
        st[kRing + 2] = w2;
        st[kRing + 3] = 0.0;
        st[kRing + 4] = w1;
        st[kRing + 5] = w2;
        st[kE1] = w2;
        st[kE2] = w1;
        overflow_ = false;
    }

    // No LTE override: the line has no charge/flux to integrate. Its
    // exactness between delayed vertices is what the breakpoints deliver;
    // the curvature at the ports is the attached dynamic devices' LTE to
    // police (see the file header). A deck driving a line from a smooth
    // source with no dynamic device anywhere samples at dtmax - the
    // known sharp edge, documented in STATUS.md.

    // §6.2: delay crossings register breakpoints. The earliest stored
    // sample time + TD strictly after t - which caps dt at TD (causality
    // of the pickup: t-TD never moves past the newest sample) and lands
    // every delayed vertex exactly on a grid point (BE restart after it,
    // §6.1). The slice arrives through the §3.5 explicit-slice convention
    // next_event grew in M4e.
    [[nodiscard]] double next_event(double t, double, double,
                                    const double* st) const override {
        if (st == nullptr) {
            return std::numeric_limits<double>::infinity();
        }
        const double td = st[kTd];
        const std::size_t count = ring_count(st);
        for (std::size_t i = 0; i < count; ++i) {
            const double e = ring_t(st, i) + td;
            if (e > t) {
                return e;
            }
        }
        return std::numeric_limits<double>::infinity();
    }

    // The §8.5 failure channel: the ring-overflow latch (set at commit,
    // the only writer) becomes the loud Timestep error here. One-shot:
    // firing clears it, the next seed clears it.
    [[nodiscard]] ws::core::Status load_error(
        const std::vector<double>&, const TranStep*, const double*,
        std::size_t) const override {
        if (!overflow_) {
            return {};
        }
        overflow_ = false;
        Diagnostic d = dev_diag(
            ErrorKind::Timestep, "spice.timestep.tline_history",
            "transmission line " + name_ + ": more than " +
                std::to_string(kRingCap) +
                " accepted steps inside one delay window - the history ring"
                " cannot hold the waveform",
            loc);
        d.hint = "reduce TD, coarsen the source edges, or loosen reltol/"
                 "chgtol so fewer steps land per delay";
        return spice_fail(std::move(d));
    }

    [[nodiscard]] bool write_card(std::string& out,
                                  const CardWriter& w) const override {
        const double* p = w.params(*this);
        out += name_ + " " + w.node(n1_) + " " + w.node(n2_) + " " + w.node(n3_) +
               " " + w.node(n4_) + " Z0=" + CardWriter::num(p[0]) +
               " Td=" + CardWriter::num(p[1]);
        if (has_ic_) {
            out += " IC=" + CardWriter::num(ic_v1_) + "," + CardWriter::num(ic_i1_) +
                   "," + CardWriter::num(ic_v2_) + "," + CardWriter::num(ic_i2_);
        }
        out += "\n";
        return true;
    }

private:
    // The KCL coupling both branch columns carry in every mode: i1 into
    // n1 / out of n2, i2 into n3 / out of n4.
    void stamp_kcl(DcLoadContext& cx) const {
        const std::size_t r1 = cx.branch_row(br1_);
        const std::size_t r2 = cx.branch_row(br2_);
        cx.add_entry(n1_, r1, 1.0);
        cx.add_entry(n2_, r1, -1.0);
        cx.add_entry(n3_, r2, 1.0);
        cx.add_entry(n4_, r2, -1.0);
    }
    void stamp_kcl(AcLoadContext& cx) const {
        const std::size_t r1 = cx.branch_row(br1_);
        const std::size_t r2 = cx.branch_row(br2_);
        cx.add_entry(n1_, r1, std::complex<double>{1.0, 0.0});
        cx.add_entry(n2_, r1, std::complex<double>{-1.0, 0.0});
        cx.add_entry(n3_, r2, std::complex<double>{1.0, 0.0});
        cx.add_entry(n4_, r2, std::complex<double>{-1.0, 0.0});
    }

    // --- ring primitives (logical index 0..count-1, physical = (head+i)%cap) --

    static std::size_t ring_count(const double* st) {
        return static_cast<std::size_t>(st[kCount]);
    }
    static std::size_t ring_head(const double* st) {
        return static_cast<std::size_t>(st[kHead]);
    }
    static double ring_t(const double* st, std::size_t i) {
        return st[kRing + 3 * ((ring_head(st) + i) % kRingCap)];
    }
    static double ring_w1(const double* st, std::size_t i) {
        return st[kRing + 3 * ((ring_head(st) + i) % kRingCap) + 1];
    }
    static double ring_w2(const double* st, std::size_t i) {
        return st[kRing + 3 * ((ring_head(st) + i) % kRingCap) + 2];
    }

    // Drops samples the next pickup can never read again: everything
    // older than the pair bracketing T - TD (the next pickup is strictly
    // beyond this one). Always keeps two.
    static void prune(double* st, double t_new) {
        const double td = st[kTd];
        const std::size_t cap = kRingCap;
        std::size_t head = ring_head(st);
        std::size_t count = ring_count(st);
        while (count > 2 &&
               st[kRing + 3 * ((head + 1) % cap)] + td <= t_new) {
            head = (head + 1) % cap;
            --count;
        }
        st[kHead] = static_cast<double>(head);
        st[kCount] = static_cast<double>(count);
    }

    void append(double* st, double t, double w1, double w2) {
        const std::size_t cap = kRingCap;
        const std::size_t count = ring_count(st);
        if (count >= cap) {
            overflow_ = true;  // load_error reports it on the next load
            return;
        }
        const std::size_t head = ring_head(st);
        const std::size_t slot = kRing + 3 * ((head + count) % cap);
        st[slot] = t;
        st[slot + 1] = w1;
        st[slot + 2] = w2;
        st[kCount] = static_cast<double>(count + 1);
    }

    std::string name_;
    NodeId n1_, n2_, n3_, n4_;
    ExprProgram z0_, td_;
    BranchId br1_, br2_;
    bool has_ic_;
    double ic_v1_, ic_i1_, ic_v2_, ic_i2_;
    std::size_t self_index_ = 0;

    // The ring-overflow latch: the one piece of run evolution that
    // cannot live in the state arena because load_error (the only load
    // failure channel, §8.5) receives no state slice. Set by commit,
    // cleared by seed_state and by its own firing. Parallel runs over
    // one shared Circuit share it; a latched overflow means the deck
    // overflows any run, so the sharing is harmless in practice (the
    // same argument as the inductor's m_ mirror).
    mutable bool overflow_ = false;
};

void TlineDevice::fold_state(double* st, const RunState& rs,
                              const Circuit& c) const {
    const double* p = rs.params().data() + c.param_span[self_index_].begin;
    st[kZ0] = p[0];
    st[kTd] = p[1];
}

}  // namespace

// --- the factory ----------------------------------------------------------------

ws::core::Result<std::unique_ptr<Device>> make_tline(const DeviceCard& card,
                                                     FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 4, 0));
    bool have_z0 = false;
    ExprProgram z0;
    bool have_td = false;
    ExprProgram td;
    bool have_f = false;
    double f = 0.0;
    double nl = 0.25;  // ngspice: NL defaults to a quarter wavelength
    bool have_ic = false;
    double ic_v1 = 0.0, ic_i1 = 0.0, ic_v2 = 0.0, ic_i2 = 0.0;
    std::size_t i = 4;
    auto bad = [&](std::string message) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.t_form",
                                   std::move(message), card.loc));
    };
    while (i < card.rest.size()) {
        const std::string lower = to_lower(card.rest[i].text);
        const std::size_t eq = lower.find('=');
        if (eq == std::string::npos) {
            return bad("transmission line " + card.name.text +
                       " takes four nodes and Z0=/TD= (or F= and NL=),"
                       " optional IC= - got '" +
                       card.rest[i].text + "'");
        }
        const std::string key = lower.substr(0, eq);
        Token val;
        val.text = card.rest[i].text.substr(eq + 1);
        val.loc = card.rest[i].loc;
        if (key == "z0") {
            if (have_z0) {
                return bad("transmission line " + card.name.text + ": Z0 twice");
            }
            WS_TRY_ASSIGN(z0, resolve_value_program(val, s.params));
            WS_TRY(eval_param_for_validation(z0, s.params));
            have_z0 = true;
            ++i;
        } else if (key == "td") {
            if (have_td) {
                return bad("transmission line " + card.name.text + ": TD twice");
            }
            WS_TRY_ASSIGN(td, resolve_value_program(val, s.params));
            WS_TRY(eval_param_for_validation(td, s.params));
            have_td = true;
            ++i;
        } else if (key == "f") {
            if (have_f) {
                return bad("transmission line " + card.name.text + ": F twice");
            }
            WS_TRY_ASSIGN(f, resolve_value_token(val, s.params));
            have_f = true;
            ++i;
        } else if (key == "nl") {
            WS_TRY_ASSIGN(nl, resolve_value_token(val, s.params));
            ++i;
        } else if (key == "ic") {
            if (have_ic) {
                return bad("transmission line " + card.name.text + ": IC twice");
            }
            have_ic = true;
            // Comma form (LTspice): IC=v1,i1,v2,i2 in one token. Space
            // form (ngspice vector): IC=v1 followed by up to three bare
            // value tokens. One to four values; the rest default to 0.
            std::vector<Token> vals;
            if (val.text.find(',') != std::string::npos) {
                std::size_t pos = 0;
                while (pos <= val.text.size()) {
                    const std::size_t comma = val.text.find(',', pos);
                    const std::string part = (comma == std::string::npos)
                                                 ? val.text.substr(pos)
                                                 : val.text.substr(pos, comma - pos);
                    if (part.empty()) {
                        return bad("transmission line " + card.name.text +
                                   ": empty IC= value");
                    }
                    Token t;
                    t.text = part;
                    t.loc = val.loc;
                    vals.push_back(std::move(t));
                    if (comma == std::string::npos) {
                        break;
                    }
                    pos = comma + 1;
                }
            } else {
                vals.push_back(val);
            }
            ++i;
            while (vals.size() < 4 && i < card.rest.size() &&
                   card.rest[i].text.find('=') == std::string::npos) {
                const ws::core::Result<double> v =
                    resolve_value_token(card.rest[i], s.params);
                if (!v) {
                    break;  // not a plain value: the next key's turn
                }
                vals.push_back(card.rest[i]);
                ++i;
            }
            if (vals.empty() || vals.size() > 4) {
                return bad("transmission line " + card.name.text +
                           ": IC= takes one to four values (V1 I1 V2 I2)");
            }
            if (vals.size() == 4 && i < card.rest.size() &&
                card.rest[i].text.find('=') == std::string::npos &&
                resolve_value_token(card.rest[i], s.params)) {
                return bad("transmission line " + card.name.text +
                           ": IC= takes one to four values (V1 I1 V2 I2)");
            }
            double num[4] = {0.0, 0.0, 0.0, 0.0};
            for (std::size_t k = 0; k < vals.size(); ++k) {
                WS_TRY_ASSIGN(num[k], resolve_value_token(vals[k], s.params));
            }
            ic_v1 = num[0];
            ic_i1 = num[1];
            ic_v2 = num[2];
            ic_i2 = num[3];
        } else {
            return bad("transmission line " + card.name.text +
                       " takes Z0=, TD= (or F= and NL=) and IC= - got '" +
                       card.rest[i].text + "'");
        }
    }
    if (!have_z0) {
        return bad("transmission line " + card.name.text +
                   " needs Z0=<characteristic impedance>");
    }
    if (have_td && have_f) {
        return bad("transmission line " + card.name.text +
                   ": give TD= or F=/NL=, not both");
    }
    if (!have_td) {
        if (!have_f) {
            return bad("transmission line " + card.name.text +
                       " needs TD=<delay> (or F= and NL=, TD = NL/F)");
        }
        if (!(f > 0.0) || !std::isfinite(f)) {
            return bad("transmission line " + card.name.text + " needs F > 0");
        }
        if (!std::isfinite(nl) || nl < 0.0) {
            return bad("transmission line " + card.name.text + " needs NL >= 0");
        }
        td = ExprProgram::constant(nl / f);
    }
    {
        // Validation doubles (the programs re-bind per run).
        WS_TRY_ASSIGN(const double z0v, eval_param_for_validation(z0, s.params));
        WS_TRY_ASSIGN(const double tdv, eval_param_for_validation(td, s.params));
        if (!(z0v > 0.0) || !std::isfinite(z0v)) {
            return bad("transmission line " + card.name.text + " needs Z0 > 0");
        }
        if (!(tdv > 0.0) || !std::isfinite(tdv)) {
            return bad("transmission line " + card.name.text +
                       " needs TD > 0 - a zero-delay line is a wire pair;"
                       " use a plain connection");
        }
    }
    // A line bridging the same node pair (either orientation) stamps a
    // degenerate zero row at DC; name it here instead of letting the
    // factorization fail generically.
    if ((n[0] == n[2] && n[1] == n[3]) || (n[0] == n[3] && n[1] == n[2])) {
        return bad("transmission line " + card.name.text +
                   " bridges the same node pair at both ports");
    }
    const BranchId br1 = s.nodes.claim_branch();
    const BranchId br2 = s.nodes.claim_branch();
    auto dev = std::make_unique<TlineDevice>(card.name.text, n[0], n[1], n[2],
                                             n[3], std::move(z0), std::move(td),
                                             br1, br2, have_ic, ic_v1, ic_i1,
                                             ic_v2, ic_i2, card.loc);
    return dev;
}

}  // namespace ws::spice
