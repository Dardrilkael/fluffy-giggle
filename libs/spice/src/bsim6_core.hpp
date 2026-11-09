#pragma once

// The BSIM6-style MOS core (level 77): a surface-potential, charge-based
// channel evaluated from the EXACT device physics - the same core idea
// Berkeley's BSIM6 builds on ("BSIM6 has a charge-based core", the 2012
// TechConnect paper), implemented here from first principles so every
// equation is gated against independent references rather than ported
// untestably.
//
// LEVEL 77 follows Xyce ("MOSFET (Level 77) BSIM6 model", the 6.3+
// reference guide) and Simetrix (BSIM-BULK: "LEVEL=77, VERSION=106"):
// ngspice ships no BSIM6 at all (verified against its NEWS/DEVICES files
// - modern compact models arrive there via OSDI/Verilog-A) and 77 is free
// in every ngspice level table. BSIM6 was renamed BSIM-BULK in 2016; the
// 6.1.x C-code lineage is what LEVEL 77 selects here.
//
// The physics (all in the MIRROR frame: NMOS-like voltages; the device
// layer flips signs for PMOS):
//
//   Space charge (Poisson + Boltzmann, both carriers, per m^2):
//     Qsc = -gamma*Cox*sqrt(PSI(psi, Vch))
//     PSI = psi + vt*(exp(-psi/vt) - 1)
//               + vt*(exp((psi - 2*phif - Vch)/vt) - exp(-2*phif/vt))
//     (holes ~ the first exponential, electrons ~ the second; both terms
//     vanish at psi = Vch = 0 so PSI(0,0) = 0 exactly - flatband clean.)
//   Gate: (Vgb' - psi)^2 = gamma^2 * PSI(psi, Vch),  Vgb' = Vgb - Vfb
//     (Vch = the local channel potential relative to the bulk Fermi level;
//     at the source end Vch = Vsb, at the drain Vch = Vsb + Vds.)
//   Charge-sheet split (smooth, exact in depletion AND strong inversion):
//     qi = -Qinv = gamma*Cox*(sqrt(PSI) - sqrt(PSI_dep)) >= 0 everywhere
//     qb = |Qdep| = gamma*Cox*sqrt(PSI_dep), PSI_dep = psi+vt*(exp(-psi/vt)-1)
//     qg = Cox*(Vgb' - psi)
//   Current (EXACT charge-sheet drift-diffusion; the surface-field drift
//   cancels against diffusion in the Boltzmann tail, leaving quasi-Fermi
//   drift - Brews/Tsividis):
//     Id = mueff*(W/L) * INTEGRAL(Vsb -> Vsb+Vdseff) qi(Vch) dVch
//   by 16-point Gauss-Legendre (nodes computed at startup, not transcribed)
//   with psi solved per node (damped Newton + analytic seeds, bisection
//   fallback). Pinch-off saturation emerges from the integral itself
//   (qi(drain end) -> 0); velocity saturation adds the finite Vdsat:
//     G_sat(V) = mueff*(W/L)*I1(V) - vsat*W*qi(Vsb+V) = 0  (monotone in V)
//     Vdseff = Vdsat - 0.5*(Vdsat-Vds + sqrt((Vdsat-Vds)^2
//                                      + 4*DELTA*Vdsat*Vds))
//   (the BSIM4-style smoothing, DELTA = 0.02: C-infinity across the knee
//   for the price of a ~(1-DELTA) triode-slope compression and a
//   sqrt(DELTA) = 14% knee softening - a documented model choice).
//   Channel-length modulation (PCLM > 0, the saturation ro layer):
//   the excess drain voltage past the knee depletes a drain-end segment
//     dL = (lc/PCLM)*Vdsat*ln(1 + Vxs/Vdsat),  lc = sqrt(3*TOX*XJ),
//     Vxs = s*softplus((Vds - Vdsat)/s),  s = DELTA*Vdsat
//   (the Litovski log law - the lateral field spreads over lc, so the
//   segment grows only logarithmically in the excess; Vdsat is the
//   pinch region's own voltage scale, the BSIM litlk analog; the
//   softplus keeps the triode untouched to ~ppm). The shortened
//   channel L - dL carries more current at the SAME saturation window,
//   and the long-channel integral reproduces that at a LARGER window:
//   Vdseff solves I1(Vsb,Vdseff) = I1(Vsb,Vdseff_smooth)*L/(L-dL)
//   (the BSIM4 fold - one window variable still drives Id and the
//   charges alike, conservation by construction; deep saturation
//   gds -> Id*(lc/PCLM)*Vdsat/(L*Vds)). PCLM <= 0 keeps the layer off
//   (the VSAT precedent - v1 decks are byte-identical); the layer
//   rides the vsat boundary, so VSAT <= 0 makes PCLM inert.
//   Charges (channel-charge-weighted averages; the partition is the
//   classic smooth chi form - 50/50 in the linear limit, 0/100 in deep
//   saturation, symmetric in S/D):
//     Qg = W*L*<qg*qi>/<qi>,   Qb = -W*L*<qb*qi>/<qi>,
//     Qch = -W*L*<qi*qi>/<qi>, chi = (qi_s-qi_d)/(qi_s+qi_d),
//     Qd = Qch*(1-chi)/2,  Qs = Qch*(1+chi)/2   (denominator floored)
//   Gate pair charges for the companions (conservation by construction:
//   qgs+qgd+qgb = Qg):
//     qgs = -Qs,  qgd = -Qd,  qgb = Qg+Qs+Qd = -Qb
//
//   EVERY derivative is analytic: dpsi/dVgb' and dpsi/dVch by implicit
//   differentiation of the gate equation, the integrals by Leibniz +
//   per-node partials, Vdsat by the implicit function theorem on G_sat,
//   Vdseff and the mobility by the chain rule. Master partials are w.r.t.
//   (Vgb', Vsb, Vds); mirror node-voltage partials follow from
//     d/dvg = d/dVgb',  d/dvd = d/dVds,
//     d/dvs = d/dVsb - d/dVds,  d/dvb = -d/dVgb' - d/dVsb.
//   (The body effect IS the integration-window shift: the integrand
//   qi(Vch) knows Vsb only through its limits - raising Vsb slides
//   [Vsb, Vsb+Vds] up qi's falling flank, exactly the classic Vth(Vsb)
//   physics.)
//
// The implemented parameter subset (BSIM4/6 names): TOX, EPSROX, NDEP
// (cm^-3), VTH0 (V; the Vgb at which psi_s = 2*phif and Vsb = 0 - Vfb
// backs out exactly), U0 (m^2/Vs), UA/UB (SI: m/V and (m/V)^2 on
// Eeff = (qb+qi/2)/eps_si in V/m - BSIM4/6 deck values do NOT transfer),
// VSAT (m/s; <= 0 disables the velocity-saturation layer), RDSW (ohm*um,
// split half per side into internal 'name#d'/'name#s' nodes),
// CGSO/CGDO (F/m, on W) / CGBO (F/m, on L), PCLM (the CLM strength,
// <= 0 off), XJ (m; only feeds lc = sqrt(3*tox*xj)), TNOM.
// Temperature: ni(T) via
// the variable-bandgap form, phif(T)/gamma(T)/Vfb(T) follow. Everything
// else a BSIM6 deck might carry is ACCEPTED but ignored (the diode-TT
// precedent) - the STATUS sharp edge lists them.

#include <cmath>
#include <cstddef>

#include "physconst.hpp"

namespace ws::spice::bsim6 {

// --- parameters (the implemented subset) ---------------------------------------

struct Params {
    double tox = 1.5e-9;       // m
    double epsrox = 3.9;
    double ndep_cm3 = 1.7e17;  // substrate doping, cm^-3 (BSIM unit)
    double vth0 = 0.6;         // V, mirror-frame positive
    double u0 = 0.04;          // m^2/(V s)
    double ua = 0.0;           // m/V    (Eeff coefficient, SI semantics)
    double ub = 0.0;           // (m/V)^2
    double vsat = 8e4;         // m/s (<= 0: no velocity saturation)
    double rdsw = 0.0;         // ohm*um (0: no series-R nodes)
    double cgso = 0.0;         // F/m
    double cgdo = 0.0;         // F/m
    double cgbo = 0.0;         // F/m
    double pclm = 0.0;         // CLM strength (<= 0: the layer is off)
    double xj = 1.5e-7;        // m (feeds lc = sqrt(3*tox*xj) only)
    double tnom_c = 27.0;
};

// --- temperature-derived quantities ---------------------------------------------

struct Derived {
    double cox = 0.023;  // F/m^2
    double gamma = 0.1;  // V^0.5
    double phif = 0.45;  // V (Fermi potential)
    double vt = 0.0259;  // V
    double vfb = 0.0;    // V (from VTH0)
    double beta2 = 0.0;  // exp(-2*phif/vt) = (ni/NA)^2
};

inline constexpr double kEps0 = 8.854214871e-12;
inline constexpr double kEpsSi = 11.7 * kEps0;

// The Vdseff smoothing constant (the BSIM4-style knee softener).
inline constexpr double kDelta = 0.02;

[[nodiscard]] inline Derived derive(const Params& p, double temp_k) {
    Derived d;
    d.vt = temp_k * phys::kKoverQ;
    const double t0 = phys::kRefTempK;
    const double vt0 = t0 * phys::kKoverQ;
    // ni(T): n_i ~ T^1.5 * exp(-Eg(T)/(2 kT)), normalized at 300.15 K
    // where ni = 1.45e16 m^-3 (the standard SPICE value).
    const double ni = 1.45e16 * std::pow(temp_k / t0, 1.5) *
                      std::exp(-0.5 * (phys::egfet(temp_k) / d.vt -
                                       phys::egfet(t0) / vt0));
    const double na = p.ndep_cm3 * 1e6;  // m^-3
    d.phif = d.vt * std::log(na / ni);
    d.cox = p.epsrox * kEps0 / p.tox;
    d.gamma = std::sqrt(2.0 * phys::kQ * kEpsSi * na) / d.cox;
    d.beta2 = std::exp(-2.0 * d.phif / d.vt);
    // VTH0 := the Vgb at which psi_s = 2*phif (Vsb = 0). The gate equation
    // gives Vgb'(psi_th) = psi_th + gamma*sqrt(PSI(psi_th, 0)), so
    // Vfb = VTH0 - psi_th - gamma*sqrt(PSI(psi_th,0)) is exact by
    // construction (the threshold row pins it: at Vgb = VTH0 the solver
    // returns psi_s = 2*phif to solver tolerance).
    const double psi_th = 2.0 * d.phif;
    const double psi_th_sc =
        psi_th + d.vt * (std::exp(-psi_th / d.vt) - 1.0) +
        d.vt * (std::exp((psi_th - 2.0 * d.phif) / d.vt) - d.beta2);
    d.vfb = p.vth0 - psi_th - d.gamma * std::sqrt(psi_th_sc);
    return d;
}

// --- the surface potential --------------------------------------------------------

struct PsiPoint {
    double psi = 0.0;
    double dpsi_dvg = 0.0;   // at fixed Vch
    double dpsi_dvch = 0.0;  // at fixed Vgb'
    bool ok = true;
};

namespace detail {

[[nodiscard]] inline double clamped_exp(double x) {
    if (x > 250.0) {
        return 3.8e108;  // exp(250); beyond any physical bias
    }
    if (x < -250.0) {
        return 2.6e-109;  // exp(-250)
    }
    return std::exp(x);
}

struct PsiSpace {
    double psi_sc = 0.0;   // PSI
    double dpsi_sc = 0.0;  // dPSI/dpsi = 1 - h + e
    double e = 0.0;        // normalized electron density exp((psi-2phif-Vch)/vt)
    double sqrt_sc = 0.0;  // sqrt(PSI)
};

[[nodiscard]] inline PsiSpace psi_space(double psi, double vch,
                                        const Derived& d) {
    PsiSpace s;
    const double h = clamped_exp(-psi / d.vt);
    s.e = clamped_exp((psi - 2.0 * d.phif - vch) / d.vt);
    s.psi_sc = psi + d.vt * (h - 1.0) + d.vt * (s.e - d.beta2);
    s.dpsi_sc = 1.0 - h + s.e;
    s.sqrt_sc = std::sqrt(s.psi_sc > 0.0 ? s.psi_sc : 0.0);
    return s;
}

[[nodiscard]] inline double softplus(double x) {
    // log(1+e^x), overflow/underflow-safe.
    if (x > 0.0) {
        return x + std::log1p(std::exp(-x));
    }
    return std::log1p(std::exp(x));
}

// 16-point Gauss-Legendre on [-1,1], COMPUTED at first use (Newton on the
// Legendre recurrence - exact to machine precision, no transcribed table
// to trust). Ascending node order, so a sweep's psi warm-start walks the
// channel monotonically.
struct Gauss16 {
    double t[16];
    double w[16];
    Gauss16() {
        const int n = 16;
        for (int i = 0; i < n; ++i) {
            double z = std::cos(3.14159265358979323846 * (i + 0.75) / (n + 0.5));
            double dp = 0.0;
            for (int it = 0; it < 100; ++it) {
                double p0 = 1.0, p1 = z;
                for (int j = 2; j <= n; ++j) {
                    const double p2 = ((2 * j - 1) * z * p1 - (j - 1) * p0) / j;
                    p0 = p1;
                    p1 = p2;
                }
                dp = n * (z * p1 - p0) / (z * z - 1.0);
                const double z1 = z;
                z = z1 - p1 / dp;
                if (std::abs(z - z1) <= 1e-15) {
                    break;
                }
            }
            // z descends with i (cos); store ascending.
            t[n - 1 - i] = -z;
            w[n - 1 - i] = 2.0 / ((1.0 - z * z) * dp * dp);
        }
    }
};

[[nodiscard]] inline const Gauss16& gauss16() {
    static const Gauss16 g;
    return g;
}

}  // namespace detail

// Solves (Vgb' - psi)^2 = gamma^2 * PSI(psi, Vch) for psi (and its
// partials by implicit differentiation of the branch-selected function
// G = Vgb' - psi - s*gamma*sqrt(PSI); s = +1 above flatband (the
// inversion/depletion root), s = -1 below (accumulation). G is strictly
// monotone on both branches, so damped Newton from a region-aware seed
// converges in a handful of steps; a bracketed bisection backs it up.
[[nodiscard]] inline PsiPoint solve_psi(double vgp, double vch,
                                        const Derived& d, double seed) {
    using detail::PsiSpace;
    using detail::psi_space;
    const double s = (vgp >= 0.0) ? 1.0 : -1.0;
    auto g_of = [&](double psi) {
        const PsiSpace sp = psi_space(psi, vch, d);
        return vgp - psi - s * d.gamma * sp.sqrt_sc;
    };
    double psi = seed;
    bool converged = false;
    for (int it = 0; it < 60; ++it) {
        const PsiSpace sp = psi_space(psi, vch, d);
        const double g = vgp - psi - s * d.gamma * sp.sqrt_sc;
        const double sq = sp.sqrt_sc > 0.0 ? sp.sqrt_sc : 1e-300;
        const double dg = -1.0 - s * d.gamma * sp.dpsi_sc / (2.0 * sq);
        if (!(std::isfinite(g) && std::isfinite(dg) && dg < 0.0)) {
            break;
        }
        double step = -g / dg;
        if (step > 2.0) {
            step = 2.0;
        }
        if (step < -2.0) {
            step = -2.0;
        }
        psi += step;
        if (std::abs(step) <= 1e-13 * (1.0 + std::abs(psi))) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        // Bracketed bisection fallback (G monotone: one sign change).
        double lo = 0.0, hi = 0.0;
        if (s > 0.0) {
            hi = std::max(seed, 0.0) + 1.0;
            while (g_of(hi) > 0.0 && hi < 1e6) {
                hi *= 2.0;
            }
        } else {
            lo = std::min(seed, 0.0) - 1.0;
            while (g_of(lo) < 0.0 && lo > -1e6) {
                lo *= 2.0;
            }
        }
        double glo = g_of(lo);
        for (int it = 0; it < 200; ++it) {
            const double mid = 0.5 * (lo + hi);
            const double gm = g_of(mid);
            if (gm == 0.0 || (hi - lo) < 1e-15 * (1.0 + std::abs(mid))) {
                psi = mid;
                converged = true;
                break;
            }
            if ((gm > 0.0) == (glo > 0.0)) {
                lo = mid;
                glo = gm;
            } else {
                hi = mid;
            }
        }
        if (!converged) {
            PsiPoint bad;
            bad.ok = false;
            return bad;  // the caller reports a Convergence error
        }
    }
    // Implicit differentiation: with G as above,
    //   dpsi/dVgb' = 1/(1 + s*gamma*dPSI/dpsi/(2 sqrt(PSI))),
    //   dpsi/dVch  = (s*gamma*e/(2 sqrt(PSI))) / (same denominator).
    const PsiSpace sp = psi_space(psi, vch, d);
    const double sq = sp.sqrt_sc > 0.0 ? sp.sqrt_sc : 1e-300;
    const double denom = 1.0 + s * d.gamma * sp.dpsi_sc / (2.0 * sq);
    PsiPoint r;
    r.psi = psi;
    r.dpsi_dvg = 1.0 / denom;
    r.dpsi_dvch = (s * d.gamma * sp.e / (2.0 * sq)) / denom;
    r.ok = std::isfinite(r.psi);
    return r;
}

// The region-aware seed: the depletion closed form blended into the
// strong-inversion asymptote by a softplus (subthreshold pins psi near
// 2*phif + Vch; strong inversion tracks Vgb' - gamma*sqrt(...)).
[[nodiscard]] inline double psi_seed(double vgp, double vch, const Derived& d) {
    if (vgp < 0.0) {
        // Accumulation: psi - vgp = gamma*sqrt(vt*exp(-psi/vt)) roughly;
        // one fixed-point step from psi0 = vgp lands in the basin.
        return vgp + 0.5 * d.gamma * std::sqrt(d.vt) * std::exp(-vgp / (2.0 * d.vt));
    }
    const double base = 2.0 * d.phif + vch;
    const double b = base > 0.01 ? base : 0.01;
    const double vov = vgp - d.gamma * std::sqrt(b);
    return b + 2.0 * d.vt * detail::softplus(vov / (2.0 * d.vt));
}

// --- one channel node: qi/qb/qg and their partials ---------------------------------

struct NodeEval {
    double psi = 0.0;       // the solved surface potential (diagnostics)
    double qi = 0.0;        // -Qinv per area (C/m^2), ~>= 0 (clamp floor)
    double dqi_dvg = 0.0;   // at fixed Vch
    double dqi_dvch = 0.0;  // at fixed Vgb'
    double qb = 0.0;        // |Qdep| per area (C/m^2)
    double dqb_dvg = 0.0;
    double dqb_dvch = 0.0;
    double qg = 0.0;        // gate sheet charge per area (C/m^2)
    double dqg_dvg = 0.0;
    double dqg_dvch = 0.0;
    bool ok = true;         // false only on a psi-solver failure
};

// The qi clamp smoothness (C/m^2): the smooth max's transition width.
// Accumulation / forward-body corners push the raw charge-sheet difference
// negative (the surface electron density dips under its bulk equilibrium
// value; the bulk path such a regime would need is outside a surface-
// potential channel model), and the smooth max keeps Id C^1 across zero.
// The width must sit far BELOW any current-relevant charge (qi ~ 1e-16
// C/m^2 still carries ~1e-21 A) yet above double ulp noise (qi ~ 1e-4 has
// ulp ~ 1e-20): 1e-27. A wider floor (1e-12 was tried) adds a spurious
// ~epsilon/2 pedestal that dominates pA-scale channels.
inline constexpr double kQiEps = 1e-27;

[[nodiscard]] inline NodeEval node_eval(double vgp, double vch,
                                        const Derived& d, double seed) {
    const PsiPoint ps = solve_psi(vgp, vch, d, seed);
    NodeEval n;
    if (!ps.ok) {
        n.ok = false;
        return n;
    }
    const double s = (vgp >= 0.0) ? 1.0 : -1.0;
    const double h = detail::clamped_exp(-ps.psi / d.vt);
    const double psi_dep = ps.psi + d.vt * (h - 1.0);
    const double sq_dep = std::sqrt(psi_dep > 0.0 ? psi_dep : 0.0);
    const double dsq_dep =
        (1.0 - h) / (2.0 * (sq_dep > 0.0 ? sq_dep : 1e-300));
    const double gc = d.gamma * d.cox;
    n.psi = ps.psi;
    // qi = Cox*s*(Vgb'-psi) - gamma*Cox*sqrt(PSI_dep)
    //    = gamma*Cox*(sqrt(PSI) - sqrt(PSI_dep))   on both branches,
    // smoothly clamped at zero (see kQiEps).
    const double qi_raw = d.cox * s * (vgp - ps.psi) - gc * sq_dep;
    const double qi_root = std::sqrt(qi_raw * qi_raw + kQiEps * kQiEps);
    const double clamp_d = 0.5 * (1.0 + qi_raw / qi_root);
    n.qi = 0.5 * (qi_raw + qi_root) - 0.5 * kQiEps;
    // dqi = d/dx [cox*s*(vgp-psi) - gc*sqrt(PSI_dep)]
    //      = -cox*s*dpsi/dx - gc*dsq_dep*dpsi/dx   (+cox*s for x=vgp).
    // The minus matters: in subthreshold the two ~cox-scale terms cancel
    // to the ~qi/(n vt) channel response (a sign slip here doubles them
    // instead - found by the FD gate at pA currents).
    const double dqi_dvg_raw =
        d.cox * s * (1.0 - ps.dpsi_dvg) - gc * dsq_dep * ps.dpsi_dvg;
    const double dqi_dvch_raw =
        d.cox * s * (-ps.dpsi_dvch) - gc * dsq_dep * ps.dpsi_dvch;
    n.dqi_dvg = clamp_d * dqi_dvg_raw;
    n.dqi_dvch = clamp_d * dqi_dvch_raw;
    n.qb = gc * sq_dep;
    n.dqb_dvg = gc * dsq_dep * ps.dpsi_dvg;
    n.dqb_dvch = gc * dsq_dep * ps.dpsi_dvch;
    n.qg = d.cox * (vgp - ps.psi);
    n.dqg_dvg = d.cox * (1.0 - ps.dpsi_dvg);
    n.dqg_dvch = d.cox * (-ps.dpsi_dvch);
    return n;
}

// --- the channel sweep -------------------------------------------------------------

// One quadrature pass over [a, a+vlim]: the current integral I1 = int qi
// dVch, the charge moments J_g = int qg*qi, J_b = int qb*qi, J_q = int
// qi*qi, each with its Vgb'-partial (the integrand carries no Vsb/Vds
// dependence - only the limits move), plus full node data at BOTH limits
// (the Leibniz terms and the partition charges). `seed_a` warm-starts the
// first psi solve; each node seeds the next.
struct Sweep {
    double i1 = 0.0, di1_dvg = 0.0;
    double jg = 0.0, djg_dvg = 0.0;
    double jb = 0.0, djb_dvg = 0.0;
    double jq = 0.0, djq_dvg = 0.0;
    NodeEval end_a, end_b;
    bool ok = true;
};

[[nodiscard]] inline Sweep channel_sweep(const Derived& d, double vgp, double a,
                                         double vlim, double seed_a) {
    Sweep sw;
    const double b = a + vlim;
    if (!(vlim > 0.0) || !(std::isfinite(vlim))) {
        const NodeEval na = node_eval(vgp, a, d, seed_a);
        if (!na.ok) {
            sw.ok = false;
            return sw;
        }
        sw.end_a = na;
        sw.end_b = na;
        return sw;
    }
    const auto& g = detail::gauss16();
    // Endpoints first (they are needed for the Leibniz terms anyway) -
    // their charge ratio picks the quadrature mapping: a decaying channel
    // (subthreshold/pinch-off, qi_b << qi_a) gets the exponential-
    // concentrating substitution, a uniform one gets plain GL16.
    const NodeEval na = node_eval(vgp, a, d, seed_a);
    if (!na.ok) {
        sw.ok = false;
        return sw;
    }
    double seed = na.psi;
    const NodeEval nb_probe = node_eval(vgp, b, d, seed);
    if (!nb_probe.ok) {
        sw.ok = false;
        return sw;
    }
    const bool decaying = nb_probe.qi < 0.05 * na.qi;
    // Quadrature mapping. A UNIFORM channel gets plain GL16 (exact to
    // solver dust on the smooth strong-inversion integrand - 1e-15 vs a
    // 4000-node Simpson at a 6x end ratio). A DECAYING one is SPLIT at
    // the c where qi crosses 0.05*qi_a (12 bisections; qi is monotone
    // down the channel): plain GL16 over [a, c] - the flat inversion
    // charge plus the knee - and the exponential-concentrating
    // substitution over [c, b]:
    //     Vch = c - lam*ln(1 - s*(1 - e^{-(b-c)/lam})), lam = 6*vt,
    // under which a ~e^{-Vch/vt} tail becomes a degree-5 polynomial
    // that GL16 integrates exactly (a linear map was 2x off at 1e-22
    // A). The split is the M8 fix: substituting over the WHOLE window
    // mapped the flat prefix onto a 1/(1-s)-type kernel that GL16
    // could not integrate (up to 6e-3 error on past-pinchoff windows,
    // non-monotone in the window length - never gated before because
    // the sweep grid's saturation windows ride the vsat layer, not the
    // pinch-off tail). Pinned at 4.6e-9 vs Simpson over the full bias
    // range, monotone to solver dust.
    seed = na.psi;
    auto accum = [&](double vch, double jac, double w) {
        const NodeEval n = node_eval(vgp, vch, d, seed);
        if (!n.ok) {
            sw.ok = false;
            return;
        }
        const double wk = 0.5 * w * jac;
        sw.i1 += wk * n.qi;
        sw.di1_dvg += wk * n.dqi_dvg;
        sw.jg += wk * n.qg * n.qi;
        sw.djg_dvg += wk * (n.dqg_dvg * n.qi + n.qg * n.dqi_dvg);
        sw.jb += wk * n.qb * n.qi;
        sw.djb_dvg += wk * (n.dqb_dvg * n.qi + n.qb * n.dqi_dvg);
        sw.jq += wk * n.qi * n.qi;
        sw.djq_dvg += wk * (2.0 * n.qi * n.dqi_dvg);
        seed = n.psi;
    };
    if (!decaying) {
        for (int i = 0; i < 16; ++i) {
            accum(a + vlim * 0.5 * (1.0 + g.t[i]), vlim, g.w[i]);
            if (!sw.ok) {
                return sw;
            }
        }
    } else {
        // The 0.05*qi_a crossing (qi monotone down the channel).
        double lo = a, hi = b;
        for (int it = 0; it < 12; ++it) {
            const double m = 0.5 * (lo + hi);
            const NodeEval nm = node_eval(vgp, m, d, psi_seed(vgp, m, d));
            if (!nm.ok) {
                sw.ok = false;
                return sw;
            }
            if (nm.qi > 0.05 * na.qi) {
                lo = m;
            } else {
                hi = m;
            }
        }
        const double c = 0.5 * (lo + hi);
        for (int i = 0; i < 16; ++i) {  // plain GL16 over [a, c]
            accum(a + (c - a) * 0.5 * (1.0 + g.t[i]), c - a, g.w[i]);
            if (!sw.ok) {
                return sw;
            }
        }
        const double lam = 6.0 * d.vt;  // substituted over [c, b]
        const double vl = b - c;
        const double one_me = 1.0 - std::exp(-vl / lam);
        for (int i = 0; i < 16; ++i) {
            const double s_n = 0.5 * (1.0 + g.t[i]);
            const double u = 1.0 - s_n * one_me;
            accum(c - lam * std::log(u), lam * one_me / u, g.w[i]);
            if (!sw.ok) {
                return sw;
            }
        }
    }
    // Exact endpoints (GL nodes are interior).
    const NodeEval nb = nb_probe;
    sw.end_a = na;
    sw.end_b = nb;
    return sw;
}

// --- the full evaluation -------------------------------------------------------------

// The result: the channel current, its mirror node-voltage partials, and
// the four terminal charges with theirs. Index order (vg, vd, vs, vb),
// all MIRROR-frame.
struct Eval {
    double id = 0.0;  // A (mirror frame: positive = NMOS-like)
    double did[4] = {};
    double qg = 0.0, qb = 0.0, qs = 0.0, qd = 0.0;  // terminal charges, C
    double dqg[4] = {}, dqb[4] = {}, dqs[4] = {}, dqd[4] = {};
    double vdseff = 0.0;
    double vdsat = 0.0;
    double qi_s = 0.0, qi_d = 0.0;  // end charges per area (diagnostics)
    bool ok = true;
};

// Options for one evaluation.
struct EvalOpts {
    bool vsat_layer = true;     // false: VSAT <= 0 (Vdseff = Vds)
    double vdsat_fixed = -1.0;  // >= 0: reuse a solved Vdsat (the dynamic
                                // hooks re-evaluate charges at a committed
                                // bias and never differentiate through it)
};

namespace detail {

// Vdseff = Vdsat - 0.5*(A + S), A = Vdsat-Vds, S = sqrt(A^2+4*DELTA*Vdsat*Vds)
// and its partials w.r.t. (Vds, Vdsat).
struct VdseffVal {
    double v = 0.0;
    double dv_dvds = 0.0;
    double dv_dvdsat = 0.0;
};

[[nodiscard]] inline VdseffVal vdseff_of(double vds, double vdsat) {
    VdseffVal r;
    const double a = vdsat - vds;
    const double s = std::sqrt(a * a + 4.0 * kDelta * vdsat * vds);
    r.v = vdsat - 0.5 * (a + s);
    r.dv_dvds = -0.5 * (-1.0 + (-a + 2.0 * kDelta * vdsat) / s);
    r.dv_dvdsat = 1.0 - 0.5 * (1.0 + (a + 2.0 * kDelta * vds) / s);
    return r;
}

// --- channel-length modulation (PCLM > 0, M8) -----------------------------------
//
// The saturation ro layer. The BSIM4 fold shape: the drain-end
// shortening is expressed as a LARGER effective drain voltage, so the
// one window variable keeps driving Id and the charges alike
// (conservation by construction). See the header comment for the law
// and its derivation; the solve follows the G_sat pattern (bracketed
// Newton, scale-relative tolerance, bisection fallback, IFT partials).
// The knee-softening scale of the excess voltage: DELTA (0.02) - a
// 7x sharper turn-on than the Vdseff smoothing's sqrt(DELTA) knee, so
// the softplus tail below the knee is negligible a few DELTA*Vdsat
// into the triode (e^{-50} at half a volt below a 0.6 V Vdsat).
inline constexpr double kClmKnee = kDelta;

[[nodiscard]] inline double clm_sigmoid(double u) {
    // 1/(1+e^-u), overflow-safe through the clamped exp.
    return 1.0 / (1.0 + clamped_exp(-u));
}

struct ClmFold {
    double v = 0.0;  // the folded Vdseff (> Vdseff_smooth in saturation)
    double dv_dvds = 0.0;   // total master partials (Vdsat chained in)
    double dv_dvsb = 0.0;
    double dv_dvg = 0.0;
    Sweep sweep;     // the channel sweep at the root (the final sweep)
    bool ok = true;  // false: a psi/solver failure (the caller goes loud)
};

// Solves the CLM fold at a solved-or-frozen Vdsat. `vsm`/`dvsm_*` are
// the DELTA-smoothed Vdseff and ITS partials (dvsm_dvsb/dvg already
// carry the ve.dv_dvdsat*vdsat_d* chain); vdsat_dvg/dvsb chain Vdsat's
// own bias dependence into dL (zero on the frozen-Vdsat path - the
// companions never differentiate through the frozen point).
[[nodiscard]] inline ClmFold clm_fold(const Params& p, const Derived& d,
                                      double vgbp, double vsb, double vds,
                                      double vdsat, double l_eff, double vsm,
                                      double dvsm_dvds, double dvsm_dvsb,
                                      double dvsm_dvg, double vdsat_dvg,
                                      double vdsat_dvsb) {
    ClmFold out;
    if (!(vdsat > 0.0)) {
        return out;  // degenerate Vdsat: keep the smoothed Vdseff
    }
    // --- dL and its (Vds, Vdsat) partials --------------------------------
    const double lc = std::sqrt(3.0 * p.tox * p.xj);
    const double s = kClmKnee * vdsat;
    const double u = (vds - vdsat) / s;
    const double sp = softplus(u);
    const double vxs = s * sp;
    const double sig = clm_sigmoid(u);
    // dVxs/dVds = sig; dVxs/dVdsat = k*sp - sig*(1 + k*u) (s scales).
    const double dvxs_dvdsat = kClmKnee * sp - sig * (1.0 + u * kClmKnee);
    const double arg = 1.0 + vxs / vdsat;
    const double dl = (lc / p.pclm) * vdsat * std::log(arg);
    // A hard clamp for silly decks (PCLM tiny AND Vds enormous): the
    // fold keeps at least a tenth of the channel. C0 only, and the
    // partials freeze on the clamped branch - never reached by sane
    // parameter sets (PCLM >= 0.05 keeps dL << L at any Vds a deck
    // can write).
    const double dl_eff = std::min(dl, 0.9 * l_eff);
    const bool clamped = dl_eff < dl;
    const double ddl_dvds = clamped ? 0.0 : (lc / p.pclm) * sig / arg;
    const double ddl_dvdsat =
        clamped ? 0.0
                : (lc / p.pclm) *
                      (std::log(arg) +
                       (dvxs_dvdsat * vdsat - vxs) / (vdsat * arg));
    const double lprime = l_eff - dl_eff;
    const double k_fac = l_eff / lprime;
    const double dk_dvds = l_eff * ddl_dvds / (lprime * lprime);
    const double dk_dvdsat = l_eff * ddl_dvdsat / (lprime * lprime);

    // --- the fold equation F(Vdseff) = I1(Vdseff) - K*I1(vsm) = 0 -------
    const Sweep sw_m = channel_sweep(d, vgbp, vsb, vsm,
                                      psi_seed(vgbp, vsb, d));
    if (!sw_m.ok) {
        out.ok = false;
        return out;
    }
    const double i1_sm = sw_m.i1;
    const double target = k_fac * i1_sm;
    auto f_of = [&](double v, Sweep* sw_out) {
        const Sweep sw = channel_sweep(d, vgbp, vsb, v,
                                        psi_seed(vgbp, vsb + 0.5 * v, d));
        if (!sw.ok) {
            return 1e300;
        }
        if (sw_out != nullptr) {
            *sw_out = sw;
        }
        return sw.i1 - target;
    };
    // Bracket: F(vsm) = I1(vsm)*(1 - K) <= 0; F(vds) must be > 0.
    Sweep sw_hi;
    const double f_hi = f_of(vds, &sw_hi);
    if (f_hi == 1e300) {
        out.ok = false;
        return out;
    }
    if (!(f_hi > 0.0)) {
        // The law asks for more current than even the FULL window
        // carries (PCLM absurdly small): pin Vdseff = Vds - the
        // graceful degradation, monotone and finite, never a failure.
        out.v = vds;
        out.dv_dvds = 1.0;
        out.sweep = sw_hi;
        return out;
    }
    double lo = vsm, hi = vds;
    double v = 0.5 * (lo + hi);
    Sweep sw_root;
    bool solved = false;
    for (int it = 0; it < 40; ++it) {
        Sweep sw;
        const double fv = f_of(v, &sw);
        if (fv == 1e300) {
            break;
        }
        const double scale = std::abs(sw.i1) + std::abs(target) + 1e-300;
        if (std::abs(fv) <= 1e-12 * scale) {
            sw_root = sw;
            solved = true;
            break;
        }
        const double fp = sw.end_b.qi;  // F'(v) = qi at the window end
        if (!(fp > 0.0)) {
            break;
        }
        double v_new = v - fv / fp;
        if (!(v_new > lo)) {
            v_new = 0.5 * (lo + v);
        }
        if (!(v_new < hi)) {
            v_new = 0.5 * (v + hi);
        }
        if (std::abs(v_new - v) <= 1e-13 * (1.0 + std::abs(v))) {
            v = v_new;
            sw_root = sw;
            solved = true;
            break;
        }
        v = v_new;
    }
    if (!solved) {
        // Bisection fallback (F monotone up on [lo, hi]).
        double glo = f_of(lo, nullptr);
        if (glo == 1e300) {
            out.ok = false;
            return out;
        }
        for (int it = 0; it < 200; ++it) {
            const double m = 0.5 * (lo + hi);
            const double gm = f_of(m, &sw_root);
            if (gm == 1e300) {
                break;
            }
            if (std::abs(hi - lo) <= 1e-13 * (1.0 + std::abs(m))) {
                v = m;
                solved = true;
                break;
            }
            if ((gm > 0.0) == (glo > 0.0)) {
                lo = m;
                glo = gm;
            } else {
                hi = m;
            }
        }
        if (!solved) {
            out.ok = false;
            return out;
        }
    }
    out.v = v;
    out.sweep = sw_root;
    // --- the IFT partials at the root ------------------------------------
    // G_x = dF/dx at the FIXED root window; dv/dx = -G_x/qi_b(root).
    const double qi_b_r = sw_root.end_b.qi;
    const double qi_a_r = sw_root.end_a.qi;
    const double qi_b_m = sw_m.end_b.qi;
    const double qi_a_m = sw_m.end_a.qi;
    if (!(std::abs(qi_b_r) > 0.0)) {
        out.ok = false;
        return out;
    }
    const double g_vds = -dk_dvds * i1_sm - k_fac * qi_b_m * dvsm_dvds;
    const double g_vg = sw_root.di1_dvg - k_fac * sw_m.di1_dvg -
                        dk_dvdsat * i1_sm * vdsat_dvg -
                        k_fac * qi_b_m * dvsm_dvg;
    const double g_vsb = (qi_b_r - qi_a_r) - k_fac * (qi_b_m - qi_a_m) -
                         k_fac * qi_b_m * dvsm_dvsb -
                         dk_dvdsat * i1_sm * vdsat_dvsb;
    out.dv_dvds = -g_vds / qi_b_r;
    out.dv_dvsb = -g_vsb / qi_b_r;
    out.dv_dvg = -g_vg / qi_b_r;
    return out;
}

}  // namespace detail

[[nodiscard]] inline Eval evaluate(const Params& p, const Derived& d, double l_eff,
                                   double w, double vgbp, double vsb, double vds,
                                   const EvalOpts& opts) {
    Eval ev;
    const bool vsat_on = opts.vsat_layer && p.vsat > 0.0;
    const double wl = w / l_eff;

    // --- mobility at the full-channel midpoint (with its partials) ---------
    // mueff = U0/(1 + UA*Eeff + UB*Eeff^2), Eeff = (qb+qi/2)/eps_si [V/m].
    // The midpoint Vch = Vsb + Vds/2 sees dVch/dVsb = 1, dVch/dVds = 1/2.
    const double vch_mid = vsb + 0.5 * vds;
    const NodeEval mid = node_eval(vgbp, vch_mid, d,
                                    psi_seed(vgbp, vch_mid, d));
    if (!mid.ok) {
        ev.ok = false;
        return ev;
    }
    const double eeff = (mid.qb + 0.5 * mid.qi) / kEpsSi;
    const double deeff_dvg = (mid.dqb_dvg + 0.5 * mid.dqi_dvg) / kEpsSi;
    const double deeff_dvch = (mid.dqb_dvch + 0.5 * mid.dqi_dvch) / kEpsSi;
    const double deeff_dvsb = deeff_dvch;
    const double deeff_dvds = 0.5 * deeff_dvch;
    const double mob_den = 1.0 + p.ua * eeff + p.ub * eeff * eeff;
    const double mueff = p.u0 / mob_den;
    // d(mueff)/dx = -U0 * (UA + 2 UB Eeff)/den^2 * dEeff/dx.
    const double mob_slope = -p.u0 * (p.ua + 2.0 * p.ub * eeff) /
                             (mob_den * mob_den);
    const double dmue_dvg = mob_slope * deeff_dvg;
    const double dmue_dvsb = mob_slope * deeff_dvsb;
    const double dmue_dvds = mob_slope * deeff_dvds;

    // --- Vdsat (the velocity-saturation boundary) ---------------------------
    double vdsat = 0.0;
    double vdsat_dvg = 0.0, vdsat_dvsb = 0.0;  // (no Vds dependence)
    if (vsat_on && opts.vdsat_fixed < 0.0) {
        // G(V) = mueff*(W/L)*I1(Vsb, V) - vsat*W*qi(Vsb+V), monotone up in V
        // (I1 grows, qi falls). Safeguarded Newton in a bracket [0, hi];
        // the root may lie beyond the applied Vds (velocity saturation not
        // reached - then Vdseff ~ Vds), so hi expands past Vds as needed.
        auto sweep_g = [&](double v, Sweep* sw_out) {
            const Sweep sw = channel_sweep(d, vgbp, vsb, v,
                                            psi_seed(vgbp, vsb + 0.5 * v, d));
            if (!sw.ok) {
                return 1e300;
            }
            if (sw_out != nullptr) {
                *sw_out = sw;
            }
            return mueff * wl * sw.i1 - p.vsat * w * sw.end_b.qi;
        };
        double lo = 0.0;
        double hi = std::max(vds, 0.5 * (vgbp - d.vfb)) + 0.5;
        while (sweep_g(hi, nullptr) < 0.0 && hi < 1e6) {
            hi *= 2.0;
        }
        double v = std::min(std::max(0.5 * vds, 0.0), hi);
        Sweep sw_root;
        bool solved = false;
        for (int it = 0; it < 40; ++it) {
            Sweep sw;
            const double gv = sweep_g(v, &sw);
            if (gv == 1e300) {
                break;
            }
            // Scale-RELATIVE tolerance: in deep subthreshold G's terms
            // are ~1e-17, and an absolute floor would stop the Newton
            // percent-early (found by the Simpson reference: Vdsat 5% off
            // at pA currents).
            const double g_scale =
                std::abs(mueff * wl * sw.i1) +
                std::abs(p.vsat * w * sw.end_b.qi) + 1e-300;
            if (std::abs(gv) <= 1e-12 * g_scale) {
                sw_root = sw;
                solved = true;
                break;
            }
            const double gp =
                mueff * wl * sw.end_b.qi - p.vsat * w * sw.end_b.dqi_dvch;
            double v_new = v - gv / gp;
            if (!(v_new > lo)) {
                v_new = 0.5 * (lo + v);
            }
            if (!(v_new < hi)) {
                v_new = 0.5 * (v + hi);
            }
            if (std::abs(v_new - v) <= 1e-13 * (1.0 + std::abs(v))) {
                v = v_new;
                sw_root = sw;
                solved = true;
                break;
            }
            v = v_new;
        }
        if (!solved) {
            // Bisection fallback on [lo, hi] (G is monotone).
            double glo = sweep_g(lo, nullptr);
            for (int it = 0; it < 200; ++it) {
                const double m = 0.5 * (lo + hi);
                const double gm = sweep_g(m, &sw_root);
                if (gm == 1e300) {
                    break;
                }
                if (std::abs(hi - lo) <= 1e-13 * (1.0 + std::abs(m))) {
                    v = m;
                    solved = true;
                    break;
                }
                if ((gm > 0.0) == (glo > 0.0)) {
                    lo = m;
                    glo = gm;
                } else {
                    hi = m;
                }
            }
            if (!solved) {
                ev.ok = false;
                return ev;
            }
        }
        vdsat = v;
        // Implicit partials: Vdsat_x = -G_x/G_V at the root, with the
        // mobility's bias dependence folded into G_x. (I1's Vsb partial
        // at fixed V is the Leibniz window shift qi_b - qi_a.)
        const Sweep& sr = sw_root;
        const double g_v = mueff * wl * sr.end_b.qi - p.vsat * w * sr.end_b.dqi_dvch;
        const double g_vgp = dmue_dvg * wl * sr.i1 + mueff * wl * sr.di1_dvg -
                             p.vsat * w * sr.end_b.dqi_dvg;
        const double g_vsb = dmue_dvsb * wl * sr.i1 +
                             mueff * wl * (sr.end_b.qi - sr.end_a.qi) -
                             p.vsat * w * sr.end_b.dqi_dvch;
        if (std::abs(g_v) > 0.0) {
            vdsat_dvg = -g_vgp / g_v;
            vdsat_dvsb = -g_vsb / g_v;
        }
    } else if (vsat_on) {
        vdsat = opts.vdsat_fixed;  // partials unused on this path
    }

    // --- Vdseff (and the CLM fold, PCLM > 0) ------------------------------------
    double vlim = vds;
    double dvlim_dvds = 1.0, dvlim_dvsb = 0.0, dvlim_dvg = 0.0;
    bool fold_sweep = false;  // the CLM fold supplied the final sweep
    Sweep swf;
    if (vsat_on) {
        const detail::VdseffVal ve = detail::vdseff_of(vds, vdsat);
        vlim = ve.v;
        dvlim_dvds = ve.dv_dvds;
        dvlim_dvsb = ve.dv_dvdsat * vdsat_dvsb;
        dvlim_dvg = ve.dv_dvdsat * vdsat_dvg;
        if (p.pclm > 0.0 && vlim > 0.0) {
            const detail::ClmFold cf = detail::clm_fold(
                p, d, vgbp, vsb, vds, vdsat, l_eff, vlim, dvlim_dvds,
                dvlim_dvsb, dvlim_dvg, vdsat_dvg, vdsat_dvsb);
            if (!cf.ok) {
                ev.ok = false;
                return ev;
            }
            vlim = cf.v;
            dvlim_dvds = cf.dv_dvds;
            dvlim_dvsb = cf.dv_dvsb;
            dvlim_dvg = cf.dv_dvg;
            swf = cf.sweep;
            fold_sweep = true;
        }
    }
    if (!(vlim > 0.0)) {
        // Vds == 0 (or the smoothing collapsed it): the channel is
        // uniform. The vlim -> 0 limits: I1 = 0, dI1/dVds = qi_a*Vdseff',
        // dI1/dVgb' = 0; the charges are the uniform-channel values with
        // their end-node partials (Vch_a = Vsb moves with Vsb, and Vds
        // moves nothing at fixed bias - only through Vdseff').
        const NodeEval na = node_eval(vgbp, vsb, d, psi_seed(vgbp, vsb, d));
        if (!na.ok) {
            ev.ok = false;
            return ev;
        }
        ev.id = 0.0;
        const double did_dvds_lim = mueff * wl * na.qi * dvlim_dvds;
        const double did_dvsb_lim = mueff * wl * na.qi * dvlim_dvsb;
        const double wl_area = w * l_eff;
        ev.qg = wl_area * na.qg;
        ev.qb = -wl_area * na.qb;
        const double qch0 = -wl_area * na.qi;
        ev.qs = 0.5 * qch0;
        ev.qd = 0.5 * qch0;
        auto scatter = [&](double dvg, double dvsb, double dvds, double out[4]) {
            out[0] = dvg;
            out[1] = dvds;
            out[2] = dvsb - dvds;
            out[3] = -dvg - dvsb;
        };
        scatter(0.0, did_dvsb_lim, did_dvds_lim, ev.did);
        scatter(wl_area * na.dqg_dvg, wl_area * na.dqg_dvch * (1.0 + dvlim_dvsb),
                wl_area * na.dqg_dvch * dvlim_dvds, ev.dqg);
        scatter(-wl_area * na.dqb_dvg, -wl_area * na.dqb_dvch * (1.0 + dvlim_dvsb),
                -wl_area * na.dqb_dvch * dvlim_dvds, ev.dqb);
        const double dqs_dvg = -0.5 * wl_area * na.dqi_dvg;
        const double dqs_dvsb = -0.5 * wl_area * na.dqi_dvch * (1.0 + dvlim_dvsb);
        const double dqs_dvds = -0.5 * wl_area * na.dqi_dvch * dvlim_dvds;
        scatter(dqs_dvg, dqs_dvsb, dqs_dvds, ev.dqs);
        scatter(dqs_dvg, dqs_dvsb, dqs_dvds, ev.dqd);
        ev.qi_s = ev.qi_d = na.qi;
        return ev;
    }
    ev.vdseff = vlim;
    ev.vdsat = vdsat;

    // --- the final sweep over the effective channel ---------------------------
    if (!fold_sweep) {
        swf = channel_sweep(d, vgbp, vsb, vlim, psi_seed(vgbp, vsb, d));
    }
    if (!swf.ok) {
        ev.ok = false;
        return ev;
    }
    const double i1 = swf.i1;
    if (!(i1 > 0.0)) {
        // Unreachable in practice (qi > 0 strictly - the electron term
        // never underflows past the exp clamp) but guarded anyway: the
        // degenerate-channel values.
        ev.qi_s = swf.end_a.qi;
        ev.qi_d = swf.end_b.qi;
        return ev;
    }

    // I1 partials (Leibniz over the moving limits - b moves with Vgb'
    // too, through Vdsat(Vgb'); the integrand's Vgb' partial is the sweep
    // column).
    const double di1_dvg = swf.di1_dvg + swf.end_b.qi * dvlim_dvg;
    const double di1_dvsb = swf.end_b.qi * (1.0 + dvlim_dvsb) - swf.end_a.qi;
    const double di1_dvds = swf.end_b.qi * dvlim_dvds;

    // Id and its master partials (mobility folded in).
    ev.id = mueff * wl * i1;
    const double did_dvg = dmue_dvg * wl * i1 + mueff * wl * di1_dvg;
    const double did_dvsb = dmue_dvsb * wl * i1 + mueff * wl * di1_dvsb;
    const double did_dvds = dmue_dvds * wl * i1 + mueff * wl * di1_dvds;

    // --- the charges ------------------------------------------------------------
    // Q = WL*J/I1 with J's Vsb/Vds partials from the moving limits only
    // (the integrand carries no Vsb/Vds dependence): dJ/dVsb = j(b)*db/dVsb
    // - j(a), dJ/dVds = j(b)*db/dVds.
    const double wl_area = w * l_eff;
    auto quotient = [&](double j, double dj_dvg, double j_end_b, double j_end_a) {
        struct Val {
            double v, dvg, dvsb, dvds;
        };
        Val q;
        q.v = wl_area * j / i1;
        const double dj_dvg_tot = dj_dvg + j_end_b * dvlim_dvg;
        q.dvg = wl_area * (dj_dvg_tot * i1 - j * di1_dvg) / (i1 * i1);
        const double dj_dvsb = j_end_b * (1.0 + dvlim_dvsb) - j_end_a;
        const double dj_dvds = j_end_b * dvlim_dvds;
        q.dvsb = wl_area * (dj_dvsb * i1 - j * di1_dvsb) / (i1 * i1);
        q.dvds = wl_area * (dj_dvds * i1 - j * di1_dvds) / (i1 * i1);
        return q;
    };
    const double jg_a = swf.end_a.qg * swf.end_a.qi;
    const double jg_b = swf.end_b.qg * swf.end_b.qi;
    const double jb_a = swf.end_a.qb * swf.end_a.qi;
    const double jb_b = swf.end_b.qb * swf.end_b.qi;
    const double jq_a = swf.end_a.qi * swf.end_a.qi;
    const double jq_b = swf.end_b.qi * swf.end_b.qi;
    const auto q_g = quotient(swf.jg, swf.djg_dvg, jg_b, jg_a);
    const auto q_b = quotient(swf.jb, swf.djb_dvg, jb_b, jb_a);
    const auto q_ch = quotient(swf.jq, swf.djq_dvg, jq_b, jq_a);

    // The partition chi = (qi_s - qi_d)/(qi_s + qi_d), denominator floored
    // (deep cutoff: both ~ the exp-clamp floor, chi of nothing is 50/50).
    const double s_sum = swf.end_a.qi + swf.end_b.qi;
    double chi = 0.0, chi_dvg = 0.0, chi_dvsb = 0.0, chi_dvds = 0.0;
    if (s_sum > 1e-30) {
        const double qa = swf.end_a.qi, qbd = swf.end_b.qi;
        // End-charge partials: the b end moves with Vgb'/Vsb/Vds through
        // Vdseff; the a end only with Vsb.
        const double da_dvg = swf.end_a.dqi_dvg;
        const double db_dvg =
            swf.end_b.dqi_dvg + swf.end_b.dqi_dvch * dvlim_dvg;
        const double da_dvsb = swf.end_a.dqi_dvch;
        const double db_dvsb = swf.end_b.dqi_dvch * (1.0 + dvlim_dvsb);
        const double db_dvds = swf.end_b.dqi_dvch * dvlim_dvds;
        chi = (qa - qbd) / s_sum;
        chi_dvg = ((da_dvg - db_dvg) * s_sum - (qa - qbd) * (da_dvg + db_dvg)) /
                  (s_sum * s_sum);
        chi_dvsb = ((da_dvsb - db_dvsb) * s_sum - (qa - qbd) * (da_dvsb + db_dvsb)) /
                   (s_sum * s_sum);
        chi_dvds = (-db_dvds * s_sum - (qa - qbd) * db_dvds) / (s_sum * s_sum);
    }

    // Terminal charges (signs: Qg > 0 in inversion, Qb < 0, Qs/Qd < 0).
    ev.qg = q_g.v;
    ev.qb = -q_b.v;
    const double qch = -q_ch.v;  // the total inversion charge (negative)
    const double qch_dvg = -q_ch.dvg, qch_dvsb = -q_ch.dvsb, qch_dvds = -q_ch.dvds;
    ev.qd = 0.5 * qch * (1.0 - chi);
    ev.qs = 0.5 * qch * (1.0 + chi);
    const double dqs_dvg = 0.5 * (qch_dvg * (1.0 + chi) + qch * chi_dvg);
    const double dqs_dvsb = 0.5 * (qch_dvsb * (1.0 + chi) + qch * chi_dvsb);
    const double dqs_dvds = 0.5 * (qch_dvds * (1.0 + chi) + qch * chi_dvds);
    const double dqd_dvg = 0.5 * (qch_dvg * (1.0 - chi) - qch * chi_dvg);
    const double dqd_dvsb = 0.5 * (qch_dvsb * (1.0 - chi) - qch * chi_dvsb);
    const double dqd_dvds = 0.5 * (qch_dvds * (1.0 - chi) - qch * chi_dvds);

    // --- master partials -> mirror node partials --------------------------------
    // d/dvg = d/dVgb'; d/dvd = d/dVds; d/dvs = d/dVsb - d/dVds;
    // d/dvb = -d/dVgb' - d/dVsb.
    auto scatter = [&](double dvg, double dvsb, double dvds, double out[4]) {
        out[0] = dvg;
        out[1] = dvds;
        out[2] = dvsb - dvds;
        out[3] = -dvg - dvsb;
    };
    scatter(did_dvg, did_dvsb, did_dvds, ev.did);
    scatter(q_g.dvg, q_g.dvsb, q_g.dvds, ev.dqg);
    scatter(-q_b.dvg, -q_b.dvsb, -q_b.dvds, ev.dqb);
    scatter(dqs_dvg, dqs_dvsb, dqs_dvds, ev.dqs);
    scatter(dqd_dvg, dqd_dvsb, dqd_dvds, ev.dqd);
    ev.qi_s = swf.end_a.qi;
    ev.qi_d = swf.end_b.qi;
    return ev;
}

}  // namespace ws::spice::bsim6
