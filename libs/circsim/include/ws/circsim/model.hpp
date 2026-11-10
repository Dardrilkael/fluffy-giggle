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
//
// Every spec carries an `unknown` object: fields a newer build wrote
// inside the entry body that this build does not know ride there and
// re-emerge on save (the .csim carrier rule, finally extended to the
// analysis entries - they used to be the one thing a round trip
// through an older build silently stripped).

struct OpSpec {
    ws::settings::json::Object unknown;
};

struct DcSpec {
    std::string source;  // "V1", "I2"
    double start = 0.0;
    double stop = 0.0;
    double step = 0.0;
    ws::settings::json::Object unknown;
};

struct TranSpec {
    double tstep = 0.0;  // 0 = tstop/100 (the ws::spice card convention)
    double tstop = 0.0;
    std::optional<double> tstart;
    std::optional<double> tmax;
    bool uic = false;
    ws::settings::json::Object unknown;
};

struct AcSpec {
    enum class Sweep { Dec, Oct, Lin };
    Sweep sweep = Sweep::Dec;
    int n = 10;
    double fstart = 1.0;
    double fstop = 1.0e6;
    ws::settings::json::Object unknown;
};

// .noise v(out[,ref]) src dec|oct|lin n fstart fstop [pts_per_summary]
// (ws::spice §7.5): the output node pair, the input source (a V or I
// element; its declared AC spec is IGNORED - the noise solve drives it
// with a unit stimulus), the frequency grid, and the per-device
// contribution summary cadence (0 = never).
struct NoiseSpec {
    enum class Sweep { Dec, Oct, Lin };
    Sweep sweep = Sweep::Dec;
    int n = 10;
    double fstart = 1.0;
    double fstop = 1.0e6;
    std::string output;  // the output net's token; exported as v(output)
    std::string ref;     // optional reference net; empty = ground
    std::string input;   // the input source's element name ("V1", "I2")
    int pts_per_summary = 0;
    ws::settings::json::Object unknown;
};

using AnalysisSpec =
    std::variant<OpSpec, DcSpec, TranSpec, AcSpec, NoiseSpec>;

struct AnalysisSetup {
    std::vector<AnalysisSpec> entries;
    // Fields a newer build wrote beside "entries" ride here (the .csim
    // carrier rule at the analysis root).
    ws::settings::json::Object unknown;

    [[nodiscard]] bool empty() const { return entries.empty(); }
};

// The shared analysis validation: the first problem with this spec's
// VALUES (zero steps, empty nodes, non-finite numbers, n < 1, reversed
// ranges), or nullopt when the spec would run. Both entry doors ride
// it - the CLI's parse_analysis_spec and the .csim loader's
// read_analysis - so an invalid analysis is rejected at entry with a
// message naming the value, not discovered as a mid-run engine error.
// (Mirrors ws::spice's own semantic checks - .dc step/direction,
// .tran's 0 <= tstart < tstop, .ac's positive-integer N and
// 0 < fstart <= fstop - one layer earlier, in circsim's vocabulary.)
[[nodiscard]] std::optional<std::string> analysis_spec_problem(
    const AnalysisSpec& a);

struct ViewState {
    double zoom = 1.0;
    Pos center{0.0, 0.0};
    ws::settings::json::Object unknown;
};

// --- the schematic ----------------------------------------------------------
//
// Concurrency: Schematic itself is NOT thread-safe - every mutation
// (add_*/erase/restore_*/the *_mut() seams/direct field writes) must
// happen-before any concurrent read, externally synchronized. Given
// that, the pure read passes (build_nets, run_erc, export_deck,
// run_schematic) may run on any number of threads at once; see
// docs/threading.md for the full contract.
class Schematic {
public:
    // A fresh id. Ids start at 1 (0 is the "none" sentinel everywhere)
    // and stop at 0xFFFFFFFE: 0xFFFFFFFF is the reserved ceiling the
    // loader rejects (see raise_next_id) so the counter can never wrap
    // a live id.
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
    // §7.7 (M5a): the run temperature, degrees C; nullopt = SPICE's
    // 27°C default. Exported as `.options temp=` ahead of the parts (a
    // `.options temp=` inside `directives` still overrides it - deck
    // order, last wins).
    std::optional<double> temp;
    ViewState view;
    ws::settings::json::Object unknown;  // unrecognized top-level fields

    [[nodiscard]] const std::deque<Part>& parts() const { return parts_; }
    [[nodiscard]] const std::deque<Junction>& junctions() const {
        return junctions_;
    }
    [[nodiscard]] const std::deque<Wire>& wires() const { return wires_; }
    [[nodiscard]] const std::deque<Label>& labels() const { return labels_; }

    // Direct container access for the editor's undo commands (the
    // writers that manage ids themselves); both must keep next_id
    // ahead of every id stored and bump() on change. THE INVARIANTS,
    // loudly: (1) after any *_mut() mutation that adds or keeps an
    // entity, next_id must be > every stored id - call raise_next_id
    // for each - or the next add_* mints a duplicate; (2) every
    // semantic change must bump() or the editor's caches (nets,
    // exports, results) serve stale data. The loader does NOT use
    // these - it rides restore_parts/restore_junctions/restore_wires/
    // restore_labels, which do both fixups themselves.
    std::deque<Part>& parts_mut() { return parts_; }
    std::deque<Junction>& junctions_mut() { return junctions_; }
    std::deque<Wire>& wires_mut() { return wires_; }
    std::deque<Label>& labels_mut() { return labels_; }

    // The loader's wholesale restore: swaps the container in, raises
    // next_id above every id it carries and bumps the revision - the
    // *_mut() invariants done for you. (set_next_id after this can
    // still raise the counter further, e.g. to honor a file's own
    // next_id; it must never lower it below a stored id.)
    void restore_parts(std::deque<Part> p);
    void restore_junctions(std::deque<Junction> j);
    void restore_wires(std::deque<Wire> w);
    void restore_labels(std::deque<Label> l);

    // Raises next_id above `id`, saturating at the reserved ceiling
    // (0xFFFFFFFF is never a valid entity id, so id + 1 can never wrap
    // - the loader rejects files that try to use it).
    void raise_next_id(std::uint32_t id) {
        if (id >= next_id_) {
            next_id_ = id + 1 == 0 ? kIdCeiling : id + 1;
        }
    }
    [[nodiscard]] std::uint32_t next_id() const { return next_id_; }
    void set_next_id(std::uint32_t v) { next_id_ = v; }

    // The largest id any file may carry; next_id saturates here.
    static constexpr std::uint32_t kIdCeiling = 0xFFFFFFFF;

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
