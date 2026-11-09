// Standalone numeric validation of src/bsim6_core.hpp (not part of the
// test binary - a development gate used while landing the core; the
// pinned rows move into test_run.cpp once the device is wired).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../libs/spice/src/bsim6_core.hpp"

using namespace ws::spice::bsim6;

static int g_fail = 0;
static void check(bool ok, const char* what, double got, double want,
                  double tol) {
    if (!ok) {
        std::printf("FAIL %-42s got %.6e want %.6e tol %.1e\n", what, got, want,
                    tol);
        ++g_fail;
    }
}

// The exact space-charge function, re-derived here independently.
static double psi_ref(double psi, double vch, const Derived& d) {
    return psi + d.vt * (std::exp(-psi / d.vt) - 1.0) +
           d.vt * (std::exp((psi - 2.0 * d.phif - vch) / d.vt) - d.beta2);
}

int main() {
    Params p;
    p.tox = 2e-9;
    p.ndep_cm3 = 2e17;
    p.vth0 = 0.5;
    p.u0 = 0.045;
    p.vsat = 8e4;
    const double temp_k = 300.15;
    const Derived d = derive(p, temp_k);
    std::printf("cox=%.4f gamma=%.4f phif=%.4f vt=%.4f vfb=%.4f\n", d.cox,
                d.gamma, d.phif, d.vt, d.vfb);

    // 1. psi residual over a dense bias grid (the solver vs the equation).
    {
        double worst = 0.0;
        int bad = 0, bisect = 0;
        for (double vgp = -2.0; vgp <= 3.0; vgp += 0.0173) {
            for (double vch = -0.3; vch <= 2.0; vch += 0.031) {
                const double seed = psi_seed(vgp, vch, d);
                const PsiPoint r = solve_psi(vgp, vch, d, seed);
                if (!r.ok) {
                    ++bad;
                    continue;
                }
                const double res =
                    std::abs(vgp * vgp - 2.0 * vgp * r.psi + r.psi * r.psi -
                             d.gamma * d.gamma * psi_ref(r.psi, vch, d));
                const double scale =
                    std::max({1.0, vgp * vgp, d.gamma * d.gamma});
                if (res / scale > worst) {
                    worst = res / scale;
                }
                if (res / scale > 1e-11) {
                    ++bad;
                }
                // dpsi partials vs central differences.
                const double h = 1e-6;
                const auto ev = [&](double dg, double dc) {
                    return solve_psi(vgp + dg, vch + dc, d, seed).psi;
                };
                const double fd_vg = (ev(h, 0) - ev(-h, 0)) / (2 * h);
                const double fd_vch = (ev(0, h) - ev(0, -h)) / (2 * h);
                const double tol = 3e-5 * (1.0 + std::abs(r.dpsi_dvg));
                if (std::abs(fd_vg - r.dpsi_dvg) > tol ||
                    std::abs(fd_vch - r.dpsi_dvch) > 3e-5 * (1.0 + std::abs(r.dpsi_dvch))) {
                    if (bisect < 5) {
                        std::printf(
                            "  dpsi FD mismatch vgp=%.3f vch=%.3f: dvg %.6f vs "
                            "%.6f, dvch %.6f vs %.6f\n",
                            vgp, vch, r.dpsi_dvg, fd_vg, r.dpsi_dvch, fd_vch);
                    }
                    ++bisect;
                }
            }
        }
        std::printf("psi residual: worst %.2e, bad %d, fd-mismatch %d\n", worst,
                    bad, bisect);
        check(worst < 1e-11, "psi residual", worst, 0.0, 1e-11);
        check(bad == 0, "psi solver failures", bad, 0, 0);
        check(bisect == 0, "dpsi FD", bisect, 0, 0);
    }

    // 2. Threshold marker: at Vgb = VTH0 (Vsb = 0) psi_s = 2*phif.
    {
        const double vgp_th = p.vth0 - d.vfb;
        const PsiPoint r = solve_psi(vgp_th, 0.0, d, psi_seed(vgp_th, 0.0, d));
        std::printf("threshold: psi(TH)=%.9f want %.9f\n", r.psi, 2 * d.phif);
        check(std::abs(r.psi - 2 * d.phif) < 1e-9, "psi at VTH0", r.psi,
              2 * d.phif, 1e-9);
    }

    const double l_eff = 1e-6, w = 1e-6;

    // The surface potential at a bias (for the swing reference).
    auto psi_s_of_mid = [&](double vgp, double vch) {
        return solve_psi(vgp, vch, d, psi_seed(vgp, vch, d)).psi;
    };

    // 3. Id vs an independent adaptive charge-sheet quadrature (validates
    // the GL16/Leibniz/Vdseff plumbing; the psi solver is shared, gated
    // separately above). Reference: Simpson over Vch with mu folded.
    {
        double worst = 0.0;
        for (double vgb = -0.1; vgb <= 2.0; vgb += 0.29) {
            for (double vsb = -0.2; vsb <= 1.0; vsb += 0.37) {
                for (double vds = 0.02; vds <= 1.6; vds += 0.41) {
                    const EvalOpts o;  // vsat layer ON (the reference below
                                       // replicates the same layer)
                    const Eval e = evaluate(p, d, l_eff, w, vgb - d.vfb, vsb,
                                            vds, o);
                    if (!e.ok) {
                        check(false, "eval ok", 0, 1, 0);
                        continue;
                    }
                    // Reference: replicate Vdseff via a slow Vdsat bisection
                    // over a Simpson-quadrature I1.
                    const double mueff = p.u0;  // ua=ub=0 here
                    auto i1_simpson = [&](double vlim) {
                        const int n = 2000;
                        const double h = vlim / n;
                        double sum = 0.0;
                        for (int i = 0; i <= n; ++i) {
                            const double vch = vsb + vlim * i / n;
                            const NodeEval nd =
                                node_eval(vgb - d.vfb, vch, d,
                                          psi_seed(vgb - d.vfb, vch, d));
                            const double c = (i == 0 || i == n)
                                                 ? 1.0
                                                 : (i % 2 == 1 ? 4.0 : 2.0);
                            sum += c * nd.qi;
                        }
                        return sum * h / 3.0;
                    };
                    auto g_of = [&](double v) {
                        return mueff * (w / l_eff) * i1_simpson(v) -
                               p.vsat * w *
                                   node_eval(vgb - d.vfb, vsb + v, d,
                                             psi_seed(vgb - d.vfb, vsb + v, d))
                                       .qi;
                    };
                    double lo = 0.0, hi = std::max(vds, 1.0);
                    while (g_of(hi) < 0.0) {
                        hi *= 2.0;
                    }
                    for (int i = 0; i < 80; ++i) {
                        const double m = 0.5 * (lo + hi);
                        if (g_of(m) < 0.0) {
                            lo = m;
                        } else {
                            hi = m;
                        }
                    }
                    const double vdsat_ref = 0.5 * (lo + hi);
                    const double a = vdsat_ref - vds;
                    const double s =
                        std::sqrt(a * a + 4.0 * kDelta * vdsat_ref * vds);
                    const double vds_eff_ref =
                        vdsat_ref - 0.5 * (a + s);
                    const double id_ref =
                        mueff * (w / l_eff) * i1_simpson(vds_eff_ref);
                    if (std::abs(id_ref) > 1e-13) {
                        // Physically relevant currents: tight relative gate.
                        const double rel =
                            std::abs(e.id - id_ref) / std::abs(id_ref);
                        if (rel > worst) {
                            worst = rel;
                        }
                        if (rel > 1e-8) {
                            std::printf(
                                "  id mismatch vgb=%.2f vsb=%.2f vds=%.2f: "
                                "%.9e vs %.9e (%.1e)\n",
                                vgb, vsb, vds, e.id, id_ref, rel);
                        }
                    } else {
                        // Below 1e-13 A (a decade under abstol) only
                        // the absolute noise floor is asserted: chasing
                        // relative agreement at 1e-19 A measures the clamp
                        // floor and Vdsat solver dust, not physics.
                        const double diff = std::abs(e.id - id_ref);
                        if (diff > 1e-15) {
                            std::printf(
                                "  id ABS mismatch vgb=%.2f vsb=%.2f vds=%.2f: "
                                "%.9e vs %.9e\n",
                                vgb, vsb, vds, e.id, id_ref);
                            worst = std::max(worst, 1.0);
                        }
                    }
                }
            }
        }
        std::printf("id vs simpson reference: worst rel %.2e (|id|>1e-15 "
                    "rows)\n", worst);
        check(worst < 1e-8, "id quadrature", worst, 0, 1e-8);
    }

    // 4. Id vs Pao-Sah (the exact double integral - the physics gate; the
    // charge-sheet approximation's gap is what it is, we pin it).
    {
        auto psi_s_of = [&](double vgp, double vch) {
            return solve_psi(vgp, vch, d, psi_seed(vgp, vch, d)).psi;
        };
        // N_sheet = int_0^psis n(psi)/E(psi) dpsi, n = (ni/NA) e^((psi-Vch)/vt),
        // E = gamma*Cox*sqrt(PSI(psi,Vch))  [per-area magnitudes].
        const double n0 = d.beta2;  // (ni/NA)^2 relative charge density
        auto n_sheet = [&](double vgp, double vch) {
            const double psis = psi_s_of(vgp, vch);
            // psi = psis * s^2 removes the 1/sqrt(psi) integrable
            // singularity at flatband (the field vanishes at psi=0).
            const int n = 4000;
            const double h = 1.0 / n;
            double sum = 0.0;
            for (int i = 0; i <= n; ++i) {
                const double s = i * h;
                const double psi = psis * s * s;
                const double psisc = psi_ref(psi, vch, d);
                const double e_den = d.gamma * d.cox *
                                     std::sqrt(psisc > 0 ? psisc : 0) /
                                     kEpsSi;
                const double integrand =
                    n0 * std::exp((psi - vch) / d.vt) /
                    std::max(e_den, 1e-30) * (2.0 * s * psis);
                const double c = (i == 0 || i == n) ? 1.0 : (i % 2 ? 4.0 : 2.0);
                sum += c * integrand;
            }
            return sum * h / 3.0;
        };
        // Id_PS = mueff*(W/L)*q*NA*N_sheet integral over Vch.
        const double q_na = 1.602176634e-19 * p.ndep_cm3 * 1e6;
        auto id_paosah = [&](double vgp, double vsb, double vds) {
            const int n = 800;
            const double h = vds / n;
            double sum = 0.0;
            for (int i = 0; i <= n; ++i) {
                const double vch = vsb + vds * i / n;
                const double c = (i == 0 || i == n) ? 1.0 : (i % 2 ? 4.0 : 2.0);
                sum += c * n_sheet(vgp, vch);
            }
            return p.u0 * (w / l_eff) * q_na * sum * h / 3.0;
        };
        double worst = 0.0;
        for (double vgb = 0.2; vgb <= 1.6; vgb += 0.35) {
            for (double vds = 0.05; vds <= 1.2; vds += 0.55) {
                // Pao-Sah has no velocity saturation: compare against the
                // vsat-off evaluation (VSAT<=0).
                Params pn = p;
                pn.vsat = 0.0;
                const Eval e = evaluate(pn, d, l_eff, w, vgb - d.vfb, 0.0,
                                        vds, EvalOpts{});
                const double ref = id_paosah(vgb - d.vfb, 0.0, vds);
                const double rel = std::abs(e.id - ref) / std::abs(ref);
                std::printf("  paosah vgb=%.2f vds=%.2f: cs %.6e ps %.6e "
                            "rel %.2e\n",
                            vgb, vds, e.id, ref, rel);
                if (rel > worst) {
                    worst = rel;
                }
            }
        }
        std::printf("id vs pao-sah: worst rel %.2e (the charge-sheet gap: "
                    "~0.6%% strong inversion growing to ~2.5%% in deep "
                    "subthreshold - the approximation's known profile)\n",
                    worst);
        check(worst < 0.03, "charge-sheet vs pao-sah gap", worst, 0, 0.03);
    }

    // 5. FD check of the full partial set (did, dq*) at several biases.
    {
        double worst = 0.0;
        const char* names[4] = {"vg", "vd", "vs", "vb"};
        for (double vgb = 0.1; vgb <= 1.8; vgb += 0.53) {
            for (double vsb = -0.1; vsb <= 0.8; vsb += 0.43) {
                for (double vds = 0.01; vds <= 1.4; vds += 0.47) {
                    // Node voltages consistent with (vgb, vsb, vds):
                    // vb = 0, vs = vsb, vg = vgb, vd = vsb + vds.
                    const double vg = vgb, vb = 0.0, vs = vsb, vd = vsb + vds;
                    const auto ev_at =
                        [&](double dvg, double dvd, double dvs, double dvb) {
                            const double vgb2 = (vg + dvg) - (vb + dvb) - d.vfb;
                            const double vsb2 = (vs + dvs) - (vb + dvb);
                            const double vds2 = (vd + dvd) - (vs + dvs);
                            return evaluate(p, d, l_eff, w, vgb2, vsb2, vds2,
                                            EvalOpts{});
                        };
                    const Eval e0 = ev_at(0, 0, 0, 0);
                    if (!e0.ok) {
                        continue;
                    }
                    const double h = 1e-6;
                    const double pert[4][4] = {
                        {h, 0, 0, 0}, {0, h, 0, 0}, {0, 0, h, 0}, {0, 0, 0, h}};
                    for (int k = 0; k < 4; ++k) {
                        const Eval ep = ev_at(pert[k][0], pert[k][1],
                                              pert[k][2], pert[k][3]);
                        const Eval em = ev_at(-pert[k][0], -pert[k][1],
                                              -pert[k][2], -pert[k][3]);
                        if (!ep.ok || !em.ok) {
                            continue;
                        }
                        const double scale = std::abs(e0.id) + 1e-9;
                        double rel =
                            std::abs((ep.id - em.id) / (2 * h) - e0.did[k]) /
                            (std::abs(e0.did[k]) + scale);
                        if (rel > worst) {
                            worst = rel;
                        }
                        const double qs_scale = std::abs(e0.qs) + 1e-18;
                        rel = std::abs((ep.qs - em.qs) / (2 * h) - e0.dqs[k]) /
                              (std::abs(e0.dqs[k]) + qs_scale);
                        if (rel > worst) {
                            worst = rel;
                        }
                        const double qg_scale = std::abs(e0.qg) + 1e-18;
                        rel = std::abs((ep.qg - em.qg) / (2 * h) - e0.dqg[k]) /
                              (std::abs(e0.dqg[k]) + qg_scale);
                        if (rel > worst) {
                            worst = rel;
                        }
                        const double qb_scale = std::abs(e0.qb) + 1e-18;
                        rel = std::abs((ep.qb - em.qb) / (2 * h) - e0.dqb[k]) /
                              (std::abs(e0.dqb[k]) + qb_scale);
                        if (rel > worst) {
                            worst = rel;
                        }
                        (void)names;
                    }
                }
            }
        }
        std::printf("FD partials (id/qg/qs/qb): worst rel %.2e\n", worst);
        check(worst < 1e-4, "FD partials", worst, 0, 1e-4);
    }

    // 6. Continuity: Id and gm vary smoothly across a fine Vgs sweep
    // crossing all regions (the SP-model selling point). The check is on
    // the RESIDUAL over the smooth increment gm*dV (a raw |dId| bound
    // would just measure the sweep's own slope).
    {
        double prev_id = 1e300, prev_gm = 1e300;
        double worst_res = 0.0, worst_gm = 0.0;
        const double dv = 0.002;
        for (double vgb = -0.2; vgb <= 2.0; vgb += dv) {
            const Eval e = evaluate(p, d, l_eff, w, vgb - d.vfb, 0.0, 0.05,
                                    EvalOpts{});
            if (!e.ok) {
                check(false, "continuity eval", 0, 1, 0);
                break;
            }
            if (prev_id < 1e299) {
                worst_res = std::max(worst_res,
                                     std::abs(e.id - prev_id - prev_gm * dv));
                worst_gm = std::max(worst_gm, std::abs(e.did[0] - prev_gm));
            }
            prev_id = e.id;
            prev_gm = e.did[0];
        }
        const double gm_scale = prev_gm;
        std::printf("continuity: max Id residual %.2e A, gm step %.2e "
                    "(2mV steps, gm ~ %.2e)\n",
                    worst_res, worst_gm, gm_scale);
        // The residual carries the sweep's own curvature
        // (0.5*gm'*dv^2 ~ 2e-10); the gm step carries gm'*dv ~ gm*dv/0.3.
        check(worst_res < 3e-9, "Id continuity", worst_res, 0, 0);
        check(worst_gm < 1e-2 * std::abs(gm_scale) + 1e-9,
              "gm continuity", worst_gm, 0, 0);
    }

    // 7. Subthreshold swing: S = ln(10)*vt*n with n = 1 + gamma/(2
    // sqrt(psi_s)) evaluated at the ACTUAL surface potential in the
    // measurement window (the 2*phif textbook limit underestimates S in
    // deep subthreshold where psi_s sits below 2*phif).
    {
        double id0 = 0, id1 = 0;
        const double vgb_a = 0.05, vgb_b = 0.15;  // well below VTH0=0.5
        const Eval ea =
            evaluate(p, d, l_eff, w, vgb_a - d.vfb, 0.0, 0.5, EvalOpts{});
        const Eval eb =
            evaluate(p, d, l_eff, w, vgb_b - d.vfb, 0.0, 0.5, EvalOpts{});
        id0 = ea.id;
        id1 = eb.id;
        const double swp = 1e3 * (vgb_b - vgb_a) /
                           (std::log10(id1) - std::log10(id0));  // mV/dec
        const double psi_mid =
            psi_s_of_mid(0.5 * (vgb_a + vgb_b) - d.vfb, 0.0);
        // n = dVgb/dpsi_s at the surface; the current's own 1/sqrt(PSI)
        // factor adds ~vt/(2 psi_s) to the swing (Id ~ qi ~ e^{psi/vt}/
        // sqrt(PSI), so d ln I/dVgb = (1/vt - PSI_psi/(2 PSI))/n).
        const double n_ref =
            1.0 + d.gamma / (2.0 * std::sqrt(psi_mid));
        const double corr = 1.0 - d.vt / (2.0 * psi_mid);
        const double want = 1e3 * std::log(10.0) * n_ref * d.vt / corr;
        std::printf("subthreshold swing: %.2f mV/dec, ref %.2f (n=%.3f, "
                    "psi_s=%.3f)\n",
                    swp, want, n_ref, psi_mid);
        check(std::abs(swp - want) / want < 0.01, "subthreshold swing", swp,
              want, 0.01);
    }

    // 8. Velocity saturation: deep-saturation Id vs vsat*W*qi_sat (the
    // boundary condition), and Vdseff clamping.
    {
        const Eval e =
            evaluate(p, d, l_eff, w, 1.5 - d.vfb, 0.0, 2.0, EvalOpts{});
        std::printf("vsat: vdsat=%.4f vdseff=%.4f id=%.6e\n", e.vdsat,
                    e.vdseff, e.id);
        check(e.vdseff < 2.0 * 0.98, "vdseff clamps", e.vdseff, 0, 0);
        check(e.id > 0, "sat id positive", e.id, 1, 0);
        // Id_sat ~ vsat*W*|Qch|/... just sanity: within an order of the
        // vsat*W*qi at the sat point.
        const NodeEval nd_sat = node_eval(
            1.5 - d.vfb, e.vdsat, d, psi_seed(1.5 - d.vfb, e.vdsat, d));
        const double id_vsat_bound = p.vsat * w * nd_sat.qi;
        std::printf("vsat bound: vsat*W*qi(sat)=%.6e vs id=%.6e\n",
                    id_vsat_bound, e.id);
        check(e.id < id_vsat_bound * 1.2, "id vs vsat bound", e.id,
              id_vsat_bound, 0.2);
    }

    std::printf(g_fail == 0 ? "ALL OK\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
