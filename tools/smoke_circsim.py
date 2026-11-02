#!/usr/bin/env python3
"""smoke_circsim.py - the circsim_cli golden-schematic smoke suite.

The two shipped examples (libs/circsim/examples) driven through the real
CLI, asserting closed-form values, exit codes and output shapes - the
"circsim_cli runnable from stage 1 on" gate. Mirrors tools/smoke_spice.py
(cross-platform Python so it runs in CI on Linux too).

  python3 tools/smoke_circsim.py --cli build/linux/bin/circsim_cli
"""

import argparse
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

    print("circsim smoke: all green")


if __name__ == "__main__":
    main()
