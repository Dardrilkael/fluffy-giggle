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
        check(r.stdout.rstrip().endswith("tline        T     4    Lossless transmission line T"),
              "symbols -S lists the library symbol")

        # The full-table JSON dump parses and carries the stage-2 fields.
        r = run(args.cli, "symbols", "-S", tline_lib, "--json")
        check(r.returncode == 0, "symbols --json exit 0")
        table = json.loads(r.stdout)
        check(table["format"] == "circsim-symbols", "json table format")
        by_key = {s["key"]: s for s in table["symbols"]}
        check(len(table["symbols"]) == 13, "12 built-ins + tline")
        check(by_key["tline"]["spice"] == "T", "tline json letter")
        check(by_key["tline"]["draw"][4] == {"rect": [-1.0, -2.0, 1.0, 2.0]},
              "tline json draw primitives")
        check(by_key["nmos"]["params"][1] ==
              {"key": "l", "type": "number", "label": "L", "default": ""},
              "nmos json param specs")

        # The quarter-wave demo: netlist golden, closed-form run, honest erc.
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

        # Without the library the demo silently loses its line: the run
        # still exits 0 (the deck is legal), but the mapped error diag
        # names the unknown symbol on stderr.
        r = run(args.cli, "run", tline, "-o", "-")
        check(r.returncode == 0, "tline without -S still runs (deck is legal)")
        check("unknown symbol 'tline'" in r.stderr,
              "tline without -S names the unknown symbol on stderr")

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

    print("circsim smoke: all green")


if __name__ == "__main__":
    main()
