#pragma once

#include <cstdint>
#include <string>

#include <ws/spice/results.hpp>

// The .raw writer (§7.8 rev, promoted from the backlog): the binary
// waveform format LTspice and ngspice viewers read. With columnar
// results it is a direct dump - one header, then the axis and signal
// columns interleaved row-wise as native doubles - which is the whole
// argument for the §7.8 storage shape: no reshape, one memcpy per point.
//
// Format (the common LTspice/ngspice subset):
//   Title: * <title>
//   Plotname: <Transient Analysis | DC Analysis | AC Analysis | ...>
//   Flags: real | complex
//   No. Variables: n
//   No. Points: p
//   Variables:
//   \t0\t<name>\t<type>
//   ...
//   Binary:
//   <p rows x n doubles (real) | p x n pairs (complex)>
//
// Determinism (§10.5): no Date/Offset lines unless the caller supplies
// them - a given deck produces byte-identical .raw output across runs.
// A .ac table's re()/im() signal pairs merge into complex columns named
// without the wrappers (v(out)), Flags: complex.

namespace ws::spice {

struct RawOptions {
    std::string title = "ws-spice";
    // Optional provenance lines; empty omits them (deterministic output).
    std::string date;
    std::string command;
};

// One table as a complete .raw document (header + binary payload).
[[nodiscard]] std::string table_raw(const Table& t, const RawOptions& opts = {});

// The first table of the run (a .raw file holds one plot; multi-analysis
// runs pick their primary - the first - and the CSV format carries all).
[[nodiscard]] std::string results_raw(const Results& r, const RawOptions& opts = {});

}  // namespace ws::spice
