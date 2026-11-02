#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ws/math/vecmath.hpp>
#include <ws/settings/json.hpp>

// The schematic data model (apps/circsim stage 1). Everything here is
// headless: no ui include, no drawing, no editor state - geometry is
// positions in grid units plus per-part rotation/mirror, and the symbol
// library (symbols.hpp) turns those into pin positions.
//
// Ids are stable uint32 values allocated from Schematic::alloc_id() and
// NEVER reused within a file: next_id is persisted, and on load it is
// raised above every id the file mentions (even one a duplicate-id repair
// dropped), so an id once retired stays retired.

namespace ws::circsim {

// A position in grid units. Y is up; rotations are CCW in that frame.
using Pos = ws::math::DVec2;

// --- wires ----------------------------------------------------------------

// A wire endpoint is a reference, not a position: it names a part pin or
// a junction. Connectivity is this reference graph plus exact position
// coincidence (see nets.hpp) - never fuzzy geometry.
struct PinRef {
    std::uint32_t part = 0;
    std::uint32_t pin = 0;  // index into the symbol's pin list

    bool operator==(const PinRef&) const = default;
};

struct JunctionRef {
    std::uint32_t junction = 0;

    bool operator==(const JunctionRef&) const = default;
};

using WireEndpoint = std::variant<PinRef, JunctionRef>;

struct Junction {
    std::uint32_t id = 0;
    Pos pos{0.0, 0.0};
    ws::settings::json::Object unknown;  // unrecognized fields, re-emitted
};

struct Wire {
    std::uint32_t id = 0;
    WireEndpoint a{};
    WireEndpoint b{};
    std::vector<Pos> waypoints;  // cosmetic bends; connectivity-free
    bool ortho = true;           // the editor's routing constraint
    ws::settings::json::Object unknown;

    // The id is assigned by Schematic::add_wire; this helper only names
    // the endpoints.
    [[nodiscard]] static Wire endpoints(WireEndpoint a, WireEndpoint b) {
        Wire w;
        w.a = a;
        w.b = b;
        return w;
    }
};

struct Label {
    std::uint32_t id = 0;
    Pos pos{0.0, 0.0};
    std::string name;  // net name; same-name labels merge, case-insensitive
    ws::settings::json::Object unknown;
};

// --- parts ----------------------------------------------------------------

struct Part {
    std::uint32_t id = 0;
    std::string symbol;  // symbol-library key: "r", "c", "npn", "gnd", ...
    std::string ref;     // "R1"; empty allowed for power/ground symbols
    std::string value;   // SPICE value text ("10k", "0"); exported verbatim
    Pos pos{0.0, 0.0};
    int rot = 0;         // degrees CCW: 0/90/180/270
    bool mirror = false; // x-flip, applied before rotation
    std::map<std::string, std::string> params;  // "model", "ac", "l", "w", ...
    ws::settings::json::Object unknown;

    [[nodiscard]] static Part make(std::string symbol_key, std::string ref,
                                   std::string value, Pos at) {
        Part p;
        p.symbol = std::move(symbol_key);
        p.ref = std::move(ref);
        p.value = std::move(value);
        p.pos = at;
        return p;
    }
};

// --- analysis setup ---------------------------------------------------------
// The schematic's stored analyses, exported as the deck's analysis cards in
// this order (deck order is run order). Same aggregates the ws::spice
// builder uses, minus the expression layer: values are numbers here, the
// property grid and the CLI write them.

struct OpSpec {};

struct DcSpec {
    std::string source;  // "V1", "I2"
    double start = 0.0;
    double stop = 0.0;
    double step = 0.0;
};

struct TranSpec {
    double tstep = 0.0;  // 0 = tstop/100 (the ws::spice card convention)
    double tstop = 0.0;
    std::optional<double> tstart;
    std::optional<double> tmax;
    bool uic = false;
};

struct AcSpec {
    enum class Sweep { Dec, Oct, Lin };
    Sweep sweep = Sweep::Dec;
    int n = 10;
    double fstart = 1.0;
    double fstop = 1.0e6;
};

using AnalysisSpec = std::variant<OpSpec, DcSpec, TranSpec, AcSpec>;

struct AnalysisSetup {
    std::vector<AnalysisSpec> entries;

    [[nodiscard]] bool empty() const { return entries.empty(); }
};

struct ViewState {
    double zoom = 1.0;
    Pos center{0.0, 0.0};
    ws::settings::json::Object unknown;
};

// --- the schematic ----------------------------------------------------------

class Schematic {
public:
    // A fresh id. Ids start at 1 (0 is the "none" sentinel everywhere).
    [[nodiscard]] std::uint32_t alloc_id() { return next_id_++; }

    // Adds with a fresh id (the incoming id field is ignored - the only
    // path that can mint ids). Returns a pointer into the storage.
    Part* add_part(Part p);
    Junction* add_junction(Junction j);
    Wire* add_wire(Wire w);
    Label* add_label(Label l);

    [[nodiscard]] const Part* find_part(std::uint32_t id) const;
    [[nodiscard]] Part* find_part(std::uint32_t id);
    [[nodiscard]] const Junction* find_junction(std::uint32_t id) const;
    [[nodiscard]] const Wire* find_wire(std::uint32_t id) const;
    [[nodiscard]] const Label* find_label(std::uint32_t id) const;

    // The change counter: every mutation through the add/erase API bumps
    // it, so caches (nets, exports, results) can detect staleness.
    [[nodiscard]] std::uint64_t version() const { return version_; }
    void bump() { ++version_; }

    // --- data (mutated by the editor's undo commands, which call bump()) ---
    double grid = 1.0;  // snap grid size in schematic units
    AnalysisSetup analysis;
    std::string directives;  // verbatim .model/.subckt/.include text block
    ViewState view;
    ws::settings::json::Object unknown;  // unrecognized top-level fields

    [[nodiscard]] const std::deque<Part>& parts() const { return parts_; }
    [[nodiscard]] const std::deque<Junction>& junctions() const {
        return junctions_;
    }
    [[nodiscard]] const std::deque<Wire>& wires() const { return wires_; }
    [[nodiscard]] const std::deque<Label>& labels() const { return labels_; }

    // Direct container access for the loader and the undo commands (the
    // two writers that manage ids themselves); both must keep next_id
    // ahead of every id stored and bump() on change.
    std::deque<Part>& parts_mut() { return parts_; }
    std::deque<Junction>& junctions_mut() { return junctions_; }
    std::deque<Wire>& wires_mut() { return wires_; }
    std::deque<Label>& labels_mut() { return labels_; }

    // Raises next_id above `id` (loader bookkeeping after tolerant reads).
    void raise_next_id(std::uint32_t id) {
        if (id >= next_id_) {
            next_id_ = id + 1;
        }
    }
    [[nodiscard]] std::uint32_t next_id() const { return next_id_; }
    void set_next_id(std::uint32_t v) { next_id_ = v; }

private:
    // Deques on purpose: references to elements stay valid across further
    // add_* calls (the editor holds Part*/Wire* across interactions), and
    // ids ride the one counter below.
    std::uint32_t next_id_ = 1;
    std::uint64_t version_ = 0;
    std::deque<Part> parts_;
    std::deque<Junction> junctions_;
    std::deque<Wire> wires_;
    std::deque<Label> labels_;
};

}  // namespace ws::circsim
