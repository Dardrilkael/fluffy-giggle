#!/usr/bin/env python3
"""Cross-platform smoke test for the spice CLI (apps/spice + libs/spice).

The golden deck battery that tools/smoke-spice.ps1 ran on Windows, now in
Python so it runs on Linux and in CI as well as on a developer's Windows
box. Each case writes a deck to a temp dir, runs the spice CLI, parses the
CSV section, and asserts closed-form values, headers, row counts and exit
codes. Every case names itself, so a regression (e.g. "the .op output was
all zeros") fails loudly with the case name, the expected value and the
offending CSV dumped to console.

Designed to be wired into ctest:

    add_test(NAME spice_smoke COMMAND python3
             ${CMAKE_SOURCE_DIR}/tools/smoke_spice.py
             --spice $<TARGET_FILE:spice>)

or run by hand:

    python3 tools/smoke_spice.py --spice build/linux/bin/spice
    python3 tools/smoke_spice.py --spice build/spice-debug/bin/spice.exe

Exit code 0 = every assertion passed; 1 = at least one failed (a summary
lists each one). The deck battery mirrors tools/smoke-spice.ps1 - keep the
two in sync when adding cases (the PowerShell script is retained for
Windows-only shops; this Python port is the CI driver).
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path


class Smoke:
    def __init__(self, spice: str, work: Path):
        self.spice = spice
        self.work = work
        self.passed = 0
        self.failed = 0
        self.failures = []
        self.last_out: list[str] = []
        self.last_err = ""

    def write_deck(self, name: str, lines: list[str], mode: str = "crlf") -> Path:
        nl = "\r\n" if mode != "lf" else "\n"
        text = nl.join(lines) + nl
        path = self.work / f"{name}.cir"
        # BOM mode = UTF-8 BOM + CRLF (Notepad authoring scenario).
        encoding_bom = mode == "bom"
        if encoding_bom:
            path.write_bytes(b"\xef\xbb\xbf" + text.encode("utf-8"))
        else:
            path.write_text(text, encoding="utf-8")
        return path

    def run_spice(self, *args: str) -> dict:
        proc = subprocess.run([self.spice, *args], capture_output=True,
                              text=True, cwd=self.work)
        self.last_out = proc.stdout.splitlines() if proc.stdout else []
        self.last_err = proc.stderr
        return {"exit": proc.returncode, "out": self.last_out, "err": proc.stderr}

    def parse_table(self, lines: list[str]) -> dict:
        start = 0
        while start < len(lines) and not lines[start].strip():
            start += 1
        headers: list[str] = []
        rows: list[list[str]] = []
        if start < len(lines):
            headers = [h.strip() for h in lines[start].split(",")]
            i = start + 1
            while i < len(lines) and lines[i].strip():
                rows.append([c.strip() for c in lines[i].split(",")])
                i += 1
        return {"headers": headers, "rows": rows}

    def col_of(self, table: dict, name: str) -> int:
        for i, h in enumerate(table["headers"]):
            if h == name:
                return i
        return -1

    def dnum(self, text: str) -> float:
        if not text or not text.strip():
            return float("nan")
        try:
            return float(text)
        except ValueError:
            return float("nan")

    def row_near(self, rows: list[list[str]], target: float) -> list[str] | None:
        best, best_diff = None, float("inf")
        for row in rows:
            d = abs(self.dnum(row[0]) - target)
            if d < best_diff:
                best_diff, best = d, row
        return best

    def expect_true(self, cond: bool, case: str, what: str) -> None:
        if cond:
            self.passed += 1
        else:
            self.failed += 1
            self.failures.append(f"{case}: {what}")

    def expect_approx(self, got: float, want: float, rtol: float,
                      case: str, what: str) -> None:
        tol = rtol * max(1.0, abs(want))
        if not (math.isfinite(got) and math.isfinite(want)) or abs(got - want) > tol:
            self.failed += 1
            self.failures.append(f"{case}: {what} - got {got}, want {want} (rtol {rtol})")
        else:
            self.passed += 1

    def expect_header(self, table: dict, want: str, case: str) -> None:
        got = ",".join(table["headers"])
        if got == want:
            self.passed += 1
        else:
            self.failed += 1
            self.failures.append(f"{case}: header is '{got}', want '{want}'")

    def case(self, name: str, body) -> None:
        before = len(self.failures)
        body()
        if len(self.failures) > before:
            print(f"  [FAIL] {name}")
            if self.last_err.strip():
                print(f"    stderr: {self.last_err.strip()[:200]}")
        else:
            print(f"  [ ok ] {name}")


def battery(s: Smoke) -> None:
    print("== analytic decks ==")

    def case_op_divider_lf():
        deck = s.write_deck("op1", [
            "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
            ".op", ".end"], mode="lf")
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "op-divider-lf",
                      f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "v(in),v(mid),i(V1)", "op-divider-lf")
        s.expect_true(len(t["rows"]) == 1, "op-divider-lf",
                      f"row count {len(t['rows'])}, want 1")
        if t["rows"]:
            row = t["rows"][0]
            s.expect_approx(s.dnum(row[0]), 5.0, 1e-9, "op-divider-lf", "v(in)")
            s.expect_approx(s.dnum(row[1]), 5.0 / 3.0, 1e-9, "op-divider-lf", "v(mid)")
            s.expect_approx(s.dnum(row[2]), -5.0 / 3000.0, 1e-9, "op-divider-lf", "i(V1)")
    s.case("op-divider-lf", case_op_divider_lf)

    def case_op_divider_crlf_bom():
        deck = s.write_deck("op2", [
            "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
            ".op", ".end"], mode="bom")
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "op-divider-crlf-bom",
                      f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "v(in),v(mid),i(V1)", "op-divider-crlf-bom")
        if t["rows"]:
            row = t["rows"][0]
            s.expect_approx(s.dnum(row[0]), 5.0, 1e-9, "op-divider-crlf-bom", "v(in)")
            s.expect_approx(s.dnum(row[1]), 5.0 / 3.0, 1e-9, "op-divider-crlf-bom", "v(mid)")
            s.expect_approx(s.dnum(row[2]), -5.0 / 3000.0, 1e-9, "op-divider-crlf-bom", "i(V1)")
    s.case("op-divider-crlf-bom", case_op_divider_crlf_bom)

    def case_op_controlled_sources():
        deck = s.write_deck("op3", [
            "controlled sources",
            "V1 a 0 1", "R1 a b 1k", "Rload b 0 2k",
            "E1 c 0 b 0 3", "R2 c d 500", "G1 d 0 a 0 0.5",
            "H1 e 0 V1 100", "R3 e 0 1k",
            "F1 0 f V1 2", "R4 f 0 100",
            ".op", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "op-ctrl", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        if t["rows"]:
            row = t["rows"][0]
            iv1 = -1.0 / 3000.0
            def col(name): return s.col_of(t, name)
            s.expect_approx(s.dnum(row[col("v(a)")]), 1.0, 1e-9, "op-ctrl", "v(a)")
            s.expect_approx(s.dnum(row[col("v(b)")]), 2.0 / 3.0, 1e-9, "op-ctrl", "v(b)")
            s.expect_approx(s.dnum(row[col("v(c)")]), 2.0, 1e-9, "op-ctrl", "v(c)")
            s.expect_approx(s.dnum(row[col("v(d)")]), -248.0, 1e-9, "op-ctrl", "v(d)")
            s.expect_approx(s.dnum(row[col("v(e)")]), 100.0 * iv1, 1e-9, "op-ctrl", "v(e)")
            s.expect_approx(s.dnum(row[col("v(f)")]), 2.0 * iv1 * 100.0, 1e-9, "op-ctrl", "v(f)")
            s.expect_approx(s.dnum(row[col("i(V1)")]), iv1, 1e-9, "op-ctrl", "i(V1)")
            s.expect_approx(s.dnum(row[col("i(E1)")]), -0.5, 1e-9, "op-ctrl", "i(E1)")
            s.expect_approx(s.dnum(row[col("i(H1)")]), -100.0 * iv1 / 1000.0, 1e-9, "op-ctrl", "i(H1)")
    s.case("op-controlled-sources", case_op_controlled_sources)

    def case_op_subckt_params():
        deck = s.write_deck("op4", [
            "amp stage",
            ".param rgain=2k",
            ".subckt divider n1 n2 n3 params: rtop=1k rbot=1k",
            "Rt n1 nm {rtop}",
            "Rb nm n2 {rbot}",
            ".ends",
            "X1 in out 0 divider params: rtop=3k rbot=1k",
            "V1 in 0 DC 4",
            "R2 out 0 5k",
            ".op", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "op-subckt", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "v(in),v(X1:nm),v(out),i(V1)", "op-subckt")
        if t["rows"]:
            row = t["rows"][0]
            s.expect_approx(s.dnum(row[0]), 4.0, 1e-9, "op-subckt", "v(in)")
            s.expect_approx(s.dnum(row[1]), 8.0 / 3.0, 1e-9, "op-subckt", "v(X1:nm)")
            s.expect_approx(s.dnum(row[2]), 20.0 / 9.0, 1e-9, "op-subckt", "v(out)")
            s.expect_approx(s.dnum(row[3]), -4.0 / 9000.0, 1e-9, "op-subckt", "i(V1)")
    s.case("op-subckt-params", case_op_subckt_params)

    def case_dc_linear():
        deck = s.write_deck("dc1", [
            "dc sweep", "V1 in 0 1", "R1 in mid 1k", "R2 mid 0 2k",
            ".dc V1 0 1.2 0.3", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "dc-linear", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "V1,v(in),v(mid),i(V1)", "dc-linear")
        s.expect_true(len(t["rows"]) == 5, "dc-linear",
                      f"row count {len(t['rows'])}, want 5")
        if len(t["rows"]) == 5:
            s.expect_approx(s.dnum(t["rows"][0][0]), 0.0, 1e-12, "dc-linear", "row 0 sweep")
            last = t["rows"][4]
            s.expect_approx(s.dnum(last[0]), 1.2, 1e-9, "dc-linear", "row 4 sweep")
            s.expect_approx(s.dnum(last[2]), 0.8, 1e-9, "dc-linear", "row 4 v(mid)")
            s.expect_approx(s.dnum(last[3]), -0.0004, 1e-9, "dc-linear", "row 4 i(V1)")
    s.case("dc-linear", case_dc_linear)

    def case_dc_nested():
        deck = s.write_deck("dc2", [
            "nested",
            "V1 in 0 DC 5", "R1 in mid 1k", "R2 mid 0 2k", "I1 mid 0 DC 0",
            ".dc V1 0 4 2 I1 0 1m 1m", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "dc-nested", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "V1,I1,v(in),v(mid),i(V1)", "dc-nested")
        s.expect_true(len(t["rows"]) == 6, "dc-nested",
                      f"row count {len(t['rows'])}, want 6")
        if len(t["rows"]) == 6:
            last = t["rows"][5]
            s.expect_approx(s.dnum(last[0]), 4.0, 1e-9, "dc-nested", "row 5 V1")
            s.expect_approx(s.dnum(last[1]), 0.001, 1e-9, "dc-nested", "row 5 I1")
            s.expect_approx(s.dnum(last[s.col_of(t, "v(mid)")]),
                            2.0, 1e-9, "dc-nested", "row 5 v(mid)")
    s.case("dc-nested", case_dc_nested)

    def case_tran_rc():
        deck = s.write_deck("tr1", [
            "rc step",
            "V1 in 0 PULSE(0 1 1u 1n 1u 10m 10m)",
            "R1 in out 1k", "C1 out 0 1u",
            ".tran 10u 5m", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "tran-rc", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "time,v(in),v(out),i(V1)", "tran-rc")
        s.expect_true(len(t["rows"]) > 10, "tran-rc",
                      f"row count {len(t['rows'])}, want > 10")
        vout = s.col_of(t, "v(out)")
        if t["rows"]:
            r1 = s.row_near(t["rows"], 0.001001)
            if r1:
                s.expect_approx(s.dnum(r1[vout]), 0.632120558828,
                                1e-3, "tran-rc", "v(out) at ~1.001ms (1 tau)")
            r3 = s.row_near(t["rows"], 0.003001)
            if r3:
                s.expect_approx(s.dnum(r3[vout]), 0.950212931632,
                                1e-3, "tran-rc", "v(out) at ~3.001ms (3 tau)")
    s.case("tran-rc", case_tran_rc)

    def case_ac_rc_lp():
        deck = s.write_deck("ac1", [
            "lp",
            "V1 in 0 DC 0 AC 1",
            "R1 in out 1k", "C1 out 0 1u",
            ".ac dec 4 1 1e6", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "ac-rc", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        s.expect_header(t, "frequency,re(v(in)),im(v(in)),re(v(out)),im(v(out)),re(i(V1)),im(i(V1))", "ac-rc")
        s.expect_true(len(t["rows"]) > 10, "ac-rc",
                      f"row count {len(t['rows'])}, want > 10")
        cre = s.col_of(t, "re(v(out))")
        cim = s.col_of(t, "im(v(out))")
        for row in t["rows"]:
            w = 2.0 * math.pi * s.dnum(row[0])
            x = w * 1e-3
            den = 1.0 + x * x
            s.expect_approx(s.dnum(row[cre]), 1.0 / den, 1e-6, "ac-rc",
                            f"re(v(out)) at f={row[0]}")
            s.expect_approx(s.dnum(row[cim]), -x / den, 1e-6, "ac-rc",
                            f"im(v(out)) at f={row[0]}")
    s.case("ac-rc-lp", case_ac_rc_lp)

    print("== output plumbing ==")

    def case_csv_file_output():
        deck = s.write_deck("op1b", [
            "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
            ".op", ".end"])
        out_path = str(s.work / "file-out.csv")
        r = s.run_spice("run", str(deck), "-o", out_path)
        s.expect_true(r["exit"] == 0, "csv-file", f"exit code {r['exit']}, want 0")
        have = os.path.exists(out_path)
        s.expect_true(have, "csv-file", "-o file was not created")
        if have:
            with open(out_path, "r", encoding="utf-8") as fh:
                lines = [ln.rstrip("\n") for ln in fh]
            t = s.parse_table(lines)
            if t["rows"]:
                s.expect_approx(s.dnum(t["rows"][0][1]), 5.0 / 3.0,
                                1e-9, "csv-file", "v(mid)")
    s.case("csv-file-output", case_csv_file_output)

    def case_include_resolution():
        s.write_deck("sub", ["Rsub out 0 1k"])
        deck = s.write_deck("inc", [
            "include main",
            "V1 in 0 5", "R1 in out 1k",
            ".include sub.cir",
            ".op", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "include", f"exit code {r['exit']}, want 0")
        t = s.parse_table(r["out"])
        if t["rows"]:
            s.expect_approx(s.dnum(t["rows"][0][s.col_of(t, "v(out)")]),
                            2.5, 1e-9, "include", "v(out)")
    s.case("include-resolution", case_include_resolution)

    def case_options_warn_never_fails():
        deck = s.write_deck("opt", [
            "options warn",
            "V1 in 0 5", "R1 in 0 1k",
            ".op", ".options bogus=1", ".end"])
        r = s.run_spice("run", str(deck))
        s.expect_true(r["exit"] == 0, "opt-warn", f"exit code {r['exit']}, want 0")
        s.expect_true(r["err"].strip() != "", "opt-warn",
                      "expected a warning on stderr, got none")
        t = s.parse_table(r["out"])
        if t["rows"]:
            s.expect_approx(s.dnum(t["rows"][0][s.col_of(t, "v(in)")]),
                            5.0, 1e-9, "opt-warn", "v(in)")
    s.case("options-warn-never-fails", case_options_warn_never_fails)

    print("== exit codes ==")

    def case_exit_codes():
        missing = s.run_spice("run", str(s.work / "does-not-exist.cir"))
        s.expect_true(missing["exit"] == 2, "exit-missing",
                      f"exit code {missing['exit']}, want 2")
        deck = s.write_deck("op1c", [
            "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
            ".op", ".end"])
        badopt = s.run_spice("run", str(deck), "-x")
        s.expect_true(badopt["exit"] == 2, "exit-badopt",
                      f"exit code {badopt['exit']}, want 2")
        bad = s.write_deck("badcard", ["bad deck", "R1 a b", ".op", ".end"])
        badrun = s.run_spice("run", str(bad))
        s.expect_true(badrun["exit"] == 1, "exit-baddeck",
                      f"exit code {badrun['exit']}, want 1")
        dir_deck = s.write_deck("direction", [
            "direction", "V1 in 0 5", "R1 in 0 1k",
            ".dc V1 0 1 -0.5", ".end"])
        dir_run = s.run_spice("run", str(dir_deck))
        s.expect_true(dir_run["exit"] == 1, "exit-dcdir",
                      f"exit code {dir_run['exit']}, want 1")
    s.case("exit-codes", case_exit_codes)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--spice", required=True,
                    help="path to the spice CLI (e.g. build/linux/bin/spice)")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep the temp deck dir for debugging")
    args = ap.parse_args()

    spice = Path(args.spice).resolve()
    if not spice.exists():
        print(f"error: spice not found at {spice}", file=sys.stderr)
        return 2

    work = Path(tempfile.gettempdir()) / f"spice-smoke-{uuid.uuid4().hex[:8]}"
    work.mkdir(parents=True, exist_ok=True)
    try:
        s = Smoke(str(spice), work)
        print(f"smoking: {spice}")
        battery(s)
        print()
        print(f"smoke: {s.passed} passed, {s.failed} failed  ({spice})")
        if s.failed:
            print()
            print("failures:")
            for f in s.failures:
                print(f"  - {f}")
            if args.keep_work:
                print(f"\ndecks kept at: {work}")
            return 1
        return 0
    finally:
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
