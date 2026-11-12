#!/usr/bin/env python3
"""smoke_circsim.py - the circsim_cli golden-schematic smoke suite.

The two shipped examples (libs/circsim/examples) driven through the real
CLI, asserting closed-form values, exit codes and output shapes - the
"circsim_cli runnable from stage 1 on" gate. Mirrors tools/smoke_spice.py
(cross-platform Python so it runs in CI on Linux too).

  python3 tools/smoke_circsim.py --cli build/linux/bin/circsim_cli
"""

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

EXAMPLES = Path(__file__).resolve().parent.parent / "libs" / "circsim" / "examples"


def run(cli, *args):
    return subprocess.run([str(cli), *[str(a) for a in args]],
                          capture_output=True, text=True)


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}", file=sys.stderr)
        sys.exit(1)
    print(f"ok: {what}")


def parse_csv(text):
    lines = [l for l in text.splitlines() if l.strip()]
    header = lines[0].split(",")
    rows = [[float(v) for v in l.split(",")] for l in lines[1:]]
    return header, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True, type=Path)
    args = ap.parse_args()

    divider = EXAMPLES / "divider.csim"
    rc = EXAMPLES / "rc_step.csim"
    tline_lib = EXAMPLES / "tline.csym"
    tline = EXAMPLES / "tline_demo.csim"

    # --- erc: both examples are clean (exit 0, no errors) ------------------
    r = run(args.cli, "erc", divider)
    check(r.returncode == 0, f"divider erc exit 0 (stderr: {r.stderr.strip()!r})")
    check("0 errors" in r.stdout, "divider erc reports zero errors")

    r = run(args.cli, "erc", rc)
    check(r.returncode == 0, "rc_step erc exit 0")

    # --- netlist: the divider's golden deck ---------------------------------
    r = run(args.cli, "netlist", divider)
    check(r.returncode == 0, "divider netlist exit 0")
    expected = (
        "circsim export\n"
        "R1 N001 out 10k\n"
        "R2 out 0 10k\n"
        "V1 N001 0 5\n"
        ".op\n"
        ".end\n"
    )
    check(r.stdout == expected, "divider golden netlist")

    # --- run: the divider's midpoint ----------------------------------------
    r = run(args.cli, "run", divider, "-o", "-")
    check(r.returncode == 0, f"divider run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check("v(out)" in header, "divider CSV carries v(out)")
    out = header.index("v(out)")
    check(len(rows) == 1, "divider .op has one row")
    check(abs(rows[0][out] - 2.5) < 1e-9,
          f"divider v(out) == 2.5 (got {rows[0][out]})")

    # --- run --analysis override: DC sweep tracks V/2 ------------------------
    r = run(args.cli, "run", divider, "-o", "-", "--analysis", "dc V1 0 5 1")
    check(r.returncode == 0, "divider dc override exit 0")
    header, rows = parse_csv(r.stdout)
    check(header[0] == "V1", "dc sweep column leads the CSV")
    check(len(rows) == 6, "dc sweep has 6 points")
    out = header.index("v(out)")
    for row in rows:
        want = row[0] / 2.0
        if abs(row[out] - want) > 1e-9:
            check(False, f"dc point {row[0]}: v(out) {row[out]} != {want}")
    check(True, "dc override tracks V/2 at every point")

    # --- run --format json ----------------------------------------------------
    r = run(args.cli, "run", divider, "-o", "-", "--format", "json")
    check(r.returncode == 0, "divider json exit 0")
    check('"format": "circsim-results"' in r.stdout, "json format marker")
    check('"v(out)"' in r.stdout, "json carries v(out)")

    # --- rc_step: the transient tracks 5*(1-exp(-t/tau)) ---------------------
    r = run(args.cli, "run", rc, "-o", "-")
    check(r.returncode == 0, f"rc_step run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check(header[0] == "time", "tran time column leads the CSV")
    t_col = header.index("time")
    out = header.index("v(out)")
    check(len(rows) > 10, "tran has more than 10 points")
    tau = 1e-3

    def near(target):
        best, best_d = None, 1e9
        for row in rows:
            d = abs(row[t_col] - target)
            if d < best_d:
                best, best_d = row, d
        return best, best_d

    for target in (3e-3, 1e-2):
        row, dist = near(target)
        check(dist < 5e-4, f"tran has a point near t={target} (off by {dist})")
        want = 5.0 * (1.0 - math.exp(-target / tau))
        check(abs(row[out] - want) < 2e-3 * 5.0,
              f"tran v(out) at ~{target} ~= {want:.4f} (got {row[out]:.4f})")

    # --- led_demo: the glow demo's three brightness steps --------------------
    # 5 V over 220/680/2.2k into LEDs (Vf ~1.7 V): the last row (the
    # settled state the editor's transport parks on) answers three
    # distinct currents - the i(Dn) probe columns the canvas glow reads.
    led = EXAMPLES / "led_demo.csim"
    r = run(args.cli, "erc", led)
    check(r.returncode == 0, "led_demo erc exit 0")

    r = run(args.cli, "run", led, "-o", "-")
    check(r.returncode == 0, f"led_demo run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check(header[0] == "time", "led_demo tran time column leads the CSV")
    d1 = header.index("i(D1)")
    d2 = header.index("i(D2)")
    d3 = header.index("i(D3)")
    last = rows[-1]
    # ~15 mA / ~5 mA / ~1.6 mA: bright, medium, dim.
    check(13e-3 < last[d1] < 17e-3, f"i(D1) ~15 mA (got {last[d1]})")
    check(4e-3 < last[d2] < 6e-3, f"i(D2) ~5 mA (got {last[d2]})")
    check(1.2e-3 < last[d3] < 2e-3, f"i(D3) ~1.6 mA (got {last[d3]})")
    check(last[d1] > last[d2] > last[d3],
          "the three LEDs land on distinct brightness steps")

    # --- the stage-24 orphan devices: the six new examples ---------------------
    # Each one exercises a built-in that ws::spice always parsed but no
    # symbol ever carried: controlled, current-controlled, behavioral,
    # switched, coupled and transformed.

    # The VCVS: v(out) = 10 * v(in) at every frequency.
    cs = EXAMPLES / "controlled.csim"
    r = run(args.cli, "erc", cs)
    check(r.returncode == 0, "controlled erc exit 0")
    r = run(args.cli, "run", cs, "-o", "-")
    check(r.returncode == 0, f"controlled run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    out = header.index("re(v(out))")
    inp = header.index("re(v(in))")
    check(len(rows) == 61, "controlled ac has 61 points")
    for row in rows:
        if abs(row[out] - 10.0 * row[inp]) > 1e-9 * 10:
            check(False, f"controlled: v(out) {row[out]} != 10*v(in) {row[inp]}")
    check(True, "controlled: the VCVS holds its x10 at every point")

    # F and H: the CCCS answers 2 * 5 mA * 1 k = 10 V, the CCVS
    # 100 ohm * 5 mA = 0.5 V (its pins flipped, so the sign reads up).
    cc = EXAMPLES / "current_controlled.csim"
    r = run(args.cli, "run", cc, "-o", "-")
    check(r.returncode == 0, "current_controlled run exit 0")
    header, rows = parse_csv(r.stdout)
    check(len(rows) == 1, "current_controlled op has one row")
    check(abs(rows[0][header.index("v(out)")] - 10.0) < 1e-9,
          "current_controlled: the CCCS answers 10 V")
    check(abs(rows[0][header.index("v(out2)")] - 0.5) < 1e-9,
          "current_controlled: the CCVS answers 0.5 V")

    # The behavioral source: V=3*v(in) -> 3 V.
    bs = EXAMPLES / "bsource.csim"
    r = run(args.cli, "run", bs, "-o", "-")
    check(r.returncode == 0, "bsource run exit 0")
    header, rows = parse_csv(r.stdout)
    check(abs(rows[0][header.index("v(out)")] - 3.0) < 1e-9,
          "bsource: V=3*v(in) answers 3 V")

    # The voltage switch: the .dc transfer curve over V2 - open below
    # Vt = 2 (the Roff divider), 5 V over 1010 above it.
    sw = EXAMPLES / "switch.csim"
    r = run(args.cli, "erc", sw)
    check(r.returncode == 0, "switch erc exit 0")
    r = run(args.cli, "run", sw, "-o", "-")
    check(r.returncode == 0, "switch run exit 0")
    header, rows = parse_csv(r.stdout)
    out = header.index("v(out)")
    check(len(rows) == 11, "switch dc sweep has 11 points")
    check(rows[0][out] < 1e-2, "switch: open at V2 = 0")
    check(rows[2][out] < 1e-2, "switch: still open at V2 = 1 (Vt = 2)")
    check(abs(rows[10][out] - 5.0 * 1000.0 / 1010.0) < 5e-3,
          "switch: Ron closed at V2 = 5")

    # The transformer part: ONE symbol, THREE cards (L1p, L1s and the K
    # that names them), and the loaded ratio k*sqrt(ls/lp) = 1.98.
    xf = EXAMPLES / "transformer.csim"
    r = run(args.cli, "erc", xf)
    check(r.returncode == 0, "transformer erc exit 0")
    r = run(args.cli, "netlist", xf)
    check(r.returncode == 0, "transformer netlist exit 0")
    for line in ("L1p pri 0 1m\n", "L1s sec 0 4m\n", "K1 L1p L1s 0.99\n"):
        check(line in r.stdout, f"transformer decks {line.strip()}")
    r = run(args.cli, "run", xf, "-o", "-")
    check(r.returncode == 0, "transformer run exit 0")
    header, rows = parse_csv(r.stdout)
    pre = header.index("re(v(pri))")
    pim = header.index("im(v(pri))")
    sre = header.index("re(v(sec))")
    sim = header.index("im(v(sec))")
    for row in rows:
        pri = math.hypot(row[pre], row[pim])
        sec = math.hypot(row[sre], row[sim])
        if abs(sec / pri - 1.98) > 0.01:
            check(False, f"transformer ratio at {row[0]}: {sec/pri} != ~1.98")
    check(True, "transformer: k*sqrt(ls/lp) = 1.98 across the band")

    # The discrete coupling: two inductors + the pinless k part, the
    # ratio k*sqrt(L2/L1) = 0.9*sqrt(10).
    cp = EXAMPLES / "coupled.csim"
    r = run(args.cli, "netlist", cp)
    check(r.returncode == 0, "coupled netlist exit 0")
    check("K1 L1 L2 0.9\n" in r.stdout, "coupled decks the K card")
    r = run(args.cli, "run", cp, "-o", "-")
    check(r.returncode == 0, "coupled run exit 0")
    header, rows = parse_csv(r.stdout)
    pre = header.index("re(v(pri))")
    pim = header.index("im(v(pri))")
    sre = header.index("re(v(sec))")
    sim = header.index("im(v(sec))")
    for row in rows:
        pri = math.hypot(row[pre], row[pim])
        sec = math.hypot(row[sre], row[sim])
        if abs(sec / pri - 0.9 * math.sqrt(10.0)) > 0.03:
            check(False, f"coupled ratio at {row[0]}: {sec/pri} != ~2.846")
    check(True, "coupled: k*sqrt(L2/L1) across the band")

    # --- the .four card (§7.9): the two-tone closed form ---------------------
    # B1 = 0.5 V @ 1 kHz + 0.1 V @ 3 kHz (the directives block carries the
    # card): fundamental 0.5 V, 3rd harmonic 0.1 V, THD exactly 20 % - the
    # whole schematic -> deck -> run -> CLI print path in one closed form.
    fo = EXAMPLES / "fourier.csim"
    r = run(args.cli, "erc", fo)
    check(r.returncode == 0, "fourier erc exit 0")
    r = run(args.cli, "run", fo, "-o", "-")
    check(r.returncode == 0, f"fourier run exit 0 (stderr: {r.stderr.strip()!r})")
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    heads = [l for l in lines if l.startswith("fourier v(out):")]
    check(len(heads) == 1, "fourier prints one v(out) header")

    def four_row(n):
        # A harmonic row: "  n  freq  magnitude  phase  normalized %" -
        # six whitespace tokens, no commas (the CSV rows are comma-joined).
        for l in lines:
            if "," in l:
                continue
            parts = l.split()
            if len(parts) == 6 and parts[0] == str(n) and parts[5] == "%":
                try:
                    return [float(p) for p in parts[1:5]]
                except ValueError:
                    pass
        return None

    h1 = four_row(1)
    h2 = four_row(2)
    h3 = four_row(3)
    check(h1 is not None, "fourier prints the harmonic rows")
    check(abs(h1[0] - 1000.0) < 1.0, f"fundamental bin at 1 kHz (got {h1[0]})")
    check(abs(h1[1] - 0.5) < 1e-3, f"fundamental magnitude 0.5 V (got {h1[1]})")
    check(h2 is not None and abs(h2[1]) < 1e-3,
          f"2nd harmonic ~0 (got {h2[1] if h2 else None})")
    check(h3 is not None and abs(h3[1] - 0.1) < 1e-3,
          f"3rd harmonic 0.1 V (got {h3[1] if h3 else None})")
    toks = heads[0].replace(",", " ").split()
    thd = float(toks[toks.index("thd") + 1])
    check(abs(thd - 20.0) < 0.05, f"fourier THD 20 % (got {thd})")

    # --- the JFET: the last classic device letter -----------------------------
    # The self-biased source follower: Id = Beta*(Vgs-Vto)^2 with
    # Vgs = -Id*Rs folds to the quadratic Rs*x^2 + x/sqrt(Beta) + Vto = 0
    # over x = sqrt(Id); the gate sits within ~1.4 uV of ground (the gmin
    # leakage through Rg), so the closed form holds to ~1e-6.
    jf = EXAMPLES / "jfet.csim"
    r = run(args.cli, "erc", jf)
    check(r.returncode == 0, "jfet erc exit 0")
    r = run(args.cli, "netlist", jf)
    check(r.returncode == 0, "jfet netlist exit 0")
    check("J1 vd N002 vs j2n5457\n" in r.stdout, "jfet decks the J card (d g s)")
    r = run(args.cli, "run", jf, "-o", "-")
    check(r.returncode == 0, f"jfet run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check(len(rows) == 1, "jfet op has one row")
    sb = 1.0 / math.sqrt(1e-4)
    x = (-sb + math.sqrt(sb * sb - 4 * 1000.0 * -2.0)) / (2 * 1000.0)
    id_ref = x * x
    vs_ref = id_ref * 1000.0
    vd_ref = 15.0 - id_ref * 4700.0
    vs = rows[0][header.index("v(vs)")]
    vd = rows[0][header.index("v(vd)")]
    ij = rows[0][header.index("i(J1)")]
    check(abs(vs - vs_ref) < 1e-6, f"jfet v(vs) = {vs_ref:.6f} (got {vs})")
    check(abs(vd - vd_ref) < 1e-5, f"jfet v(vd) = {vd_ref:.6f} (got {vd})")
    check(abs(ij - id_ref) < 1e-8, f"jfet i(J1) = {id_ref:.9f} (got {ij})")

    # --- the zener: the diode Bv/Ibv breakdown ---------------------------------
    # The shunt regulator with IBV set to the exact bias current
    # (15.1-5.1)/1k = 10 mA pins the rail at Bv EXACTLY - the knee solve
    # passes -IBV at -Bv by construction.
    zn = EXAMPLES / "zener.csim"
    r = run(args.cli, "erc", zn)
    check(r.returncode == 0, "zener erc exit 0")
    r = run(args.cli, "netlist", zn)
    check(r.returncode == 0, "zener netlist exit 0")
    check("D1 0 out zd5v1\n" in r.stdout, "zener decks the cathode-to-rail D card")
    check("Bv=5.1 Ibv=10m" in r.stdout, "zener decks the Bv/Ibv model")
    r = run(args.cli, "run", zn, "-o", "-")
    check(r.returncode == 0, f"zener run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    out = rows[0][header.index("v(out)")]
    check(abs(out - 5.1) < 1e-9, f"zener pins v(out) at 5.1 V (got {out})")
    check(abs(rows[0][header.index("i(D1)")] + 0.01) < 1e-9,
          "zener sinks exactly the 10 mA bias")


    # --- the built-in model library (stage 25) -------------------------------
    # A semiconductor part's `model` param names a card and the card rides
    # the deck with no directives block - the two shipped examples prove
    # the whole path (pick -> export -> run) on closed forms.

    # The fixed-bias BJT stage: Rb 1meg from the rail, the 2n3904 card's
    # own Gummel-Poon numbers answer. beta_forced = Bf*(1+(Vce-Vbe)/Vaf)
    # is the card's forward relation at the bias point.
    ba = EXAMPLES / "bjt_amp.csim"
    r = run(args.cli, "erc", ba)
    check(r.returncode == 0, "bjt_amp erc exit 0")
    check("0 errors, 0 warnings" in r.stdout, "bjt_amp erc fully clean")
    r = run(args.cli, "netlist", ba)
    check(r.returncode == 0, "bjt_amp netlist exit 0")
    check(".model 2n3904 NPN(Is=6.734f Bf=206" in r.stdout,
          "bjt_amp decks the library's 2n3904 card")
    r = run(args.cli, "run", ba, "-o", "-")
    check(r.returncode == 0, f"bjt_amp run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check(len(rows) == 1, "bjt_amp op has one row")
    vc = rows[0][header.index("v(vc)")]
    vbe = rows[0][header.index("v(N002)")]
    isup = rows[0][header.index("i(V1)")]  # -(Ic + Ib) when delivering
    check(3.5 < vc < 4.5, f"bjt_amp biases mid-rail: v(vc) ~ 4 V (got {vc})")
    ib = (12.0 - vbe) / 1e6
    ic = -isup - ib
    beta = ic / ib
    want = 206.0 * (1.0 + (vc - vbe) / 74.0)  # Bf*(1+(Vce-Vbe)/Vaf)
    check(abs(beta - want) / want < 1e-3,
          f"bjt_amp beta_forced = {want:.2f} (got {beta:.2f})")

    # The non-inverting op amp stage: gain 1 + Rf/Rg = 10 through the
    # ua741 card's linear macromodel - the FINITE-gain closed form
    # 10*Avol/(10+Avol), not the wishful 10.
    oa = EXAMPLES / "opamp_amp.csim"
    r = run(args.cli, "erc", oa)
    check(r.returncode == 0, "opamp_amp erc exit 0")
    check("0 errors, 0 warnings" in r.stdout, "opamp_amp erc fully clean")
    r = run(args.cli, "netlist", oa)
    check(r.returncode == 0, "opamp_amp netlist exit 0")
    for line in (".subckt ua741 out inp inn vp vm", "Eo b1 0 g2 0 2e5",
                 ".ends"):
        check(line in r.stdout, f"opamp_amp decks the macromodel ({line[:16]})")
    xline = next(l for l in r.stdout.splitlines() if l.startswith("X1 "))
    check(xline.startswith("X1 out ") and xline.endswith(" ua741"),
          f"opamp_amp decks the X card in port order ({xline})")
    r = run(args.cli, "run", oa, "-o", "-")
    check(r.returncode == 0, f"opamp_amp run exit 0 (stderr: {r.stderr.strip()!r})")
    header, rows = parse_csv(r.stdout)
    check(len(rows) == 1, "opamp_amp op has one row")
    vout = rows[0][header.index("v(out)")]
    vfb = rows[0][header.index("v(N004)")]  # the in- feedback node
    iv1 = rows[0][header.index("i(V1)")]
    want = 10.0 * 2e5 / (10.0 + 2e5)  # 9.9995: gain 10, Avol 2e5
    check(abs(vout - want) / want < 1e-6,
          f"opamp_amp v(out) = {want} (got {vout})")
    check(abs(vfb - vout / 10.0) < 1e-6,
          f"opamp_amp feedback node rides the divider (got {vfb})")
    check(abs(iv1) < 1e-9, f"opamp_amp input draws nothing (got {iv1})")

    # The models listing: the shelf itself, table and JSON.
    r = run(args.cli, "models")
    check(r.returncode == 0, "models exit 0")
    names = [l.split()[0] for l in r.stdout.splitlines()[1:] if l.strip()]
    check(len(names) == 25, f"the shelf carries 25 cards (got {len(names)})")
    check("1n4148" in names and "ua741" in names and "2n3904" in names,
          "the shelf lists the diode/BJT/opamp cards")
    r = run(args.cli, "models", "--json")
    check(r.returncode == 0, "models --json exit 0")
    table = json.loads(r.stdout)
    check(table["format"] == "circsim-models", "models json format marker")
    by_name = {m["name"]: m for m in table["models"]}
    check(by_name["1n4148"]["kind"] == "d" and
          ".model 1n4148 D(" in by_name["1n4148"]["text"],
          "models json: the diode card")
    check(by_name["ua741"]["kind"] == "opamp" and
          by_name["ua741"]["text"].startswith(".subckt ua741 "),
          "models json: the opamp subckt card")

    # --- a broken file exits 1 with a mapped problem -------------------------
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "bad.csim"
        bad.write_text(
            '{"format": "circsim", "version": 1, "grid": 1.0, "next_id": 5,'
            ' "parts": ['
            '{"id": 1, "symbol": "v", "ref": "V1", "value": "5",'
            ' "pos": [0.0, -4.0], "rot": 0, "mirror": false, "params": {}},'
            '{"id": 2, "symbol": "v", "ref": "V2", "value": "3",'
            ' "pos": [8.0, 0.0], "rot": 0, "mirror": false, "params": {}},'
            '{"id": 3, "symbol": "gnd", "ref": "", "value": "",'
            ' "pos": [4.0, -8.0], "rot": 0, "mirror": false, "params": {}}'
            '], "junctions": [], "wires": ['
            '{"id": 4, "a": {"pin": {"part": 1, "pin": 0}},'
            ' "b": {"pin": {"part": 2, "pin": 0}}, "waypoints": [],'
            ' "ortho": true},'
            '{"id": 5, "a": {"pin": {"part": 1, "pin": 1}},'
            ' "b": {"pin": {"part": 2, "pin": 1}}, "waypoints": [],'
            ' "ortho": true}'
            '], "labels": [], "analysis": {"entries": [{"op": {}}]}}'
        )
        r = run(args.cli, "run", bad, "-o", "-")
        check(r.returncode == 1, "vsource-loop run exits 1")
        check("V1" in r.stderr or "V2" in r.stderr,
              "vsource-loop error maps to a part ref")

        r = run(args.cli, "erc", bad)
        check(r.returncode == 1, "vsource-loop erc exits 1")

        missing = Path(tmp) / "nope.csim"
        r = run(args.cli, "run", missing)
        check(r.returncode == 2, "missing file exits 2")

    # --- the stage-2 symbol library: -S, symbols, the override path ------------
    with tempfile.TemporaryDirectory() as tmp:
        # The symbols listing starts from the built-ins and grows with -S.
        r = run(args.cli, "symbols")
        check(r.returncode == 0, "symbols exit 0")
        check(r.stdout.splitlines()[1].split()[0] == "r",
              "symbols lists the built-ins first")
        r = run(args.cli, "symbols", "-S", tline_lib)
        check(r.returncode == 0, "symbols -S exit 0")
        # tline is a BUILT-IN since stage 24: the example now overrides
        # it in place (warned), so the row sits in the built-in block,
        # not appended at the end.
        check(any(line.split() and line.split()[0] == "tline"
                  for line in r.stdout.splitlines()),
              "symbols -S lists the tline row")
        check("redefines the built-in 'tline'" in r.stderr,
              "the tline override warns on stderr")

        # The full-table JSON dump parses and carries the stage-2 fields.
        r = run(args.cli, "symbols", "-S", tline_lib, "--json")
        check(r.returncode == 0, "symbols --json exit 0")
        table = json.loads(r.stdout)
        check(table["format"] == "circsim-symbols", "json table format")
        by_key = {s["key"]: s for s in table["symbols"]}
        # 24 built-ins + tline (stage 24: the orphan devices; the JFET
        # pair njf/pjf joined next) + the op amp (stage 25); the tline
        # override keeps the built-in's slot.
        check(len(table["symbols"]) == 26, "the widened built-in table")
        for key, letter in (("e", "E"), ("f", "F"), ("g", "G"), ("h", "H"),
                            ("b", "B"), ("s", "S"), ("w", "W"), ("k", "K"),
                            ("xfmr", "K"), ("tline", "T"), ("opamp", "X")):
            check(by_key[key]["spice"] == letter,
                  f"the {key} built-in carries its {letter} letter")
        check(by_key["tline"]["spice"] == "T", "tline json letter")
        check(by_key["tline"]["draw"][4] == {"rect": [-1.0, -2.0, 1.0, 2.0]},
              "tline json draw primitives")
        check(by_key["nmos"]["params"][1] ==
              {"key": "l", "type": "number", "label": "L", "default": ""},
              "nmos json param specs")
        # The transformer's multi-card layout round-trips through the
        # JSON dump: three cards, the coupling naming its siblings.
        cards = by_key["xfmr"]["cards"]
        check(len(cards) == 3, "xfmr json cards")
        check(cards[0]["letter"] == "L" and cards[0]["suffix"] == "p",
              "the primary card")
        check(cards[2]["letter"] == "K" and cards[2]["name_siblings"],
              "the coupling card names its siblings")

        # The stage-17 headless render: every symbol through the render
        # seam as SVG - one file per key plus the contact sheet. The
        # structural contract only (existence, svg header, pin marks);
        # the geometry lives in test_symbol_render's cases.
        svg_dir = Path(tmp) / "svg"
        r = run(args.cli, "symbols", "-S", tline_lib, "--svg", svg_dir)
        check(r.returncode == 0, "symbols --svg exit 0")
        for key in ("r", "v", "gnd", "vcc", "npn", "tline", "opamp"):
            f = svg_dir / f"{key}.svg"
            check(f.is_file() and f.stat().st_size > 0,
                  f"svg written: {key}.svg")
        sheet = (svg_dir / "symbols.svg").read_text()
        check(sheet.startswith("<?xml"), "svg sheet is xml")
        check(all(f">{k} (" in sheet or f">{k.upper()} (" in sheet
                  or k in sheet for k in ("Resistor", "Ground", "Supply")),
              "svg sheet captions the symbols")
        one = (svg_dir / "r.svg").read_text()
        check(one.count("#b4443c") == 4,
              "svg marks both pins (a and b dots + names)")

        # The quarter-wave demo: netlist golden, closed-form run, honest erc.
        # The tline is a built-in now, so -S merely overrides it with the
        # same geometry - both spellings of the run carry the same deck.
        r = run(args.cli, "netlist", tline, "-S", tline_lib)
        check(r.returncode == 0, "tline netlist exit 0")
        expected = (
            "circsim export\n"
            "Rs in a 50\n"
            "T1 a 0 b 0 Z0=50 TD=1u\n"
            "V1 in 0 1 AC 1\n"
            ".ac lin 3 250000 750000\n"
            ".end\n"
        )
        check(r.stdout == expected, "tline golden netlist through the library")
        r = run(args.cli, "netlist", tline)
        check(r.stdout == expected,
              "tline golden netlist on the built-in alone (stage 24)")

        r = run(args.cli, "run", tline, "-S", tline_lib, "-o", "-")
        check(r.returncode == 0, f"tline run exit 0 (stderr: {r.stderr.strip()!r})")
        header, rows = parse_csv(r.stdout)
        va = header.index("re(v(a))")
        check(len(rows) == 3, "tline ac has 3 points")
        for row, want in zip(rows, (0.0, 1.0, 0.0)):
            if abs(row[va] - want) > 0.05:
                check(False, f"tline re(v(a)) at {row[0]}: {row[va]} != ~{want}")
        check(True, "tline quarter-wave: short, open, short")

        r = run(args.cli, "erc", tline, "-S", tline_lib)
        check(r.returncode == 0, "tline erc exit 0 (open port warns, not errors)")
        check("0 errors" in r.stdout, "tline erc zero errors")
        check("net b has only one connection" in r.stdout,
              "tline erc warns about the open port")

        # Without the library the built-in answers the same demo now
        # (stage 24): the unknown-symbol escape hatch is closed, the run
        # carries the line either way.
        r = run(args.cli, "run", tline, "-o", "-")
        check(r.returncode == 0, "tline runs on the built-in alone")
        header, rows = parse_csv(r.stdout)
        check(header[0] == "frequency", "tline ac CSV leads with frequency")
        va = header.index("re(v(a))")
        check(len(rows) == 3, "tline ac has 3 points")
        for row, want in zip(rows, (0.0, 1.0, 0.0)):
            if abs(row[va] - want) > 0.05:
                check(False, f"tline re(v(a)) at {row[0]}: {row[va]} != ~{want}")
        check(True, "tline quarter-wave on the built-in: short, open, short")

        # A library override: redefining a built-in warns and wins (the
        # definition REPLACES the built-in wholesale - spice/ref included).
        wide = Path(tmp) / "wide.csym"
        wide.write_text(
            '{"format": "circsim-symbols", "version": 1,'
            ' "symbols": [{"key": "r", "name": "Wide resistor", "spice": "R",'
            ' "ref": "R",'
            ' "pins": [{"name": "a", "pos": [-4.0, 0.0], "dir": 180},'
            ' {"name": "b", "pos": [4.0, 0.0], "dir": 0}],'
            ' "value": true, "emit": []}]}'
        )
        r = run(args.cli, "symbols", "-S", wide)
        check("redefines the built-in 'r'" in r.stderr,
              "override warns on stderr")
        r = run(args.cli, "netlist", divider, "-S", wide)
        check(r.returncode == 0, "divider netlist through the override")
        check("R1 N001 out 10k" in r.stdout,
              "override keeps the divider exporting (reference edges)")

        # A broken library fails loudly with exit 2.
        badlib = Path(tmp) / "bad.csym"
        badlib.write_text("{not json")
        r = run(args.cli, "symbols", "-S", badlib)
        check(r.returncode == 2, "broken library exits 2")

        r = run(args.cli, "run", tline, "-S")
        check(r.returncode == 2, "-S without a file exits 2")

        # --- png: the raster export (png.hpp) ----------------------------
        # Python's zlib is the only decoder near the build: inflating
        # IDAT verifies the writer's fixed-Huffman deflate bit for bit,
        # and the defiltered scanlines carry the palette counts.
        # Determinism makes the sheets diffable across builds.
        import struct
        import zlib

        def parse_png(data):
            assert data[:8] == b"\x89PNG\r\n\x1a\n", "png signature"
            out = []
            i = 8
            while i + 12 <= len(data):
                (n,) = struct.unpack(">I", data[i:i + 4])
                ctype = data[i + 4:i + 8]
                body = data[i + 8:i + 8 + n]
                (crc,) = struct.unpack(">I", data[i + 8 + n:i + 12 + n])
                assert crc == (zlib.crc32(ctype + body) & 0xFFFFFFFF), \
                    f"{ctype} crc mismatch"
                out.append((ctype, body))
                i += 12 + n
                if ctype == b"IEND":
                    break
            assert i == len(data), "trailing bytes after IEND"
            return out

        def png_pixels(chunks):
            w, h = struct.unpack(">II", chunks[0][1][:8])
            assert chunks[0][1][8:13] == b"\x08\x02\x00\x00\x00", "8-bit RGB"
            raw = zlib.decompress(b"".join(b for t, b in chunks
                                           if t == b"IDAT"))
            assert len(raw) == h * (1 + 3 * w), "scanline size"
            rows = []
            for y in range(h):
                base = y * (1 + 3 * w)
                assert raw[base] == 0, "filter byte is 0"
                row = raw[base + 1:base + 1 + 3 * w]
                rows.append([tuple(row[x * 3:x * 3 + 3]) for x in range(w)])
            return w, h, rows

        def runb(cli, *args):  # binary stdout: no text decoding
            return subprocess.run([str(cli), *[str(a) for a in args]],
                                  capture_output=True)

        ink = (0x1F, 0x24, 0x30)      # symbols, wires, dots
        label_c = (0xB0, 0x56, 0x00)  # net labels
        annot_c = (0x7A, 0x26, 0x00)  # ref/value annotations

        sheet = Path(tmp) / "divider.png"
        r = run(args.cli, "png", divider, "-o", sheet)
        check(r.returncode == 0,
              f"divider png exit 0 (stderr: {r.stderr.strip()!r})")
        w, h, rows = png_pixels(parse_png(sheet.read_bytes()))
        check(200 < w < 2000 and 100 < h < 2000, f"divider png {w}x{h}")
        check(rows[0][0] == (255, 255, 255) and rows[-1][-1] == (255, 255, 255),
              "divider png corners are white")
        n_ink = sum(row.count(ink) for row in rows)
        n_lab = sum(row.count(label_c) for row in rows)
        n_ann = sum(row.count(annot_c) for row in rows)
        check(n_ink > 300, f"divider png ink pixels {n_ink}")
        check(n_lab > 20, f"divider png label pixels {n_lab}")
        check(n_ann > 20, f"divider png annotation pixels {n_ann}")

        again = Path(tmp) / "divider2.png"
        run(args.cli, "png", divider, "-o", again)
        check(again.read_bytes() == sheet.read_bytes(),
              "divider png is byte-deterministic")

        small = Path(tmp) / "divider8.png"
        run(args.cli, "png", divider, "-o", small, "--ppu", "8")
        w8, h8, _ = png_pixels(parse_png(small.read_bytes()))
        check(w8 < w and h8 < h, f"ppu 8 shrinks the sheet ({w8}x{h8})")

        # The big one: the SCR trigger sheet inflates, and every
        # quadrant of the canvas carries ink.
        scr = EXAMPLES / "scr_trigger_vfmm.csim"
        big = Path(tmp) / "scr.png"
        r = run(args.cli, "png", scr, "-o", big)
        check(r.returncode == 0,
              f"scr png exit 0 (stderr: {r.stderr.strip()!r})")
        sw, sh, srows = png_pixels(parse_png(big.read_bytes()))
        s_ink = sum(row.count(ink) for row in srows)
        check(s_ink > 5000, f"scr png ink pixels {s_ink}")
        for name, (x0, y0) in {"TL": (0, 0), "TR": (sw // 2, 0),
                               "BL": (0, sh // 2),
                               "BR": (sw // 2, sh // 2)}.items():
            n = sum(1 for y in range(y0, y0 + sh // 2, 3)
                    for x in range(x0, x0 + sw // 2, 3)
                    if srows[y][x] == ink)
            check(n > 50, f"scr png {name} quadrant has ink ({n})")

        # A .csym-layered sheet, binary stdout, and the guardrails.
        r = run(args.cli, "png", tline, "-S", tline_lib, "-o",
                Path(tmp) / "tline.png")
        check(r.returncode == 0, "tline png through the library exit 0")
        r = runb(args.cli, "png", divider, "-o", "-")
        check(r.returncode == 0 and r.stdout[:8] == b"\x89PNG\r\n\x1a\n",
              "png -o - writes the signature to stdout")
        for label, argv in (
                ("png without -o", ["png", divider]),
                ("ppu below 4", ["png", divider, "-o",
                                 str(Path(tmp) / "x.png"), "--ppu", "3"]),
                ("ppu above 96", ["png", divider, "-o",
                                  str(Path(tmp) / "x.png"), "--ppu", "100"]),
                ("valid ppu still needs -o", ["png", divider, "--ppu", "24"])):
            r = run(args.cli, *argv)
            check(r.returncode == 2, f"usage error exits 2: {label}")

    print("circsim smoke: all green")


if __name__ == "__main__":
    main()
