// The stage-2 symbol library: .csym load/save, layering (built-ins +
// overrides), the typed param specs, the draw primitives, and the ERC
// checks they enable - plus the full schematic -> deck -> run path
// through a library-defined symbol.

#include <cmath>
#include <string>
#include <vector>

// LOCAL SANDBOX NOTE (review build only, not a repo change): GCC 14's
// -Wmaybe-uninitialized fires a known false positive when a
// default-constructed AnalysisSpec variant is moved into a vector at
// -O3; MSVC (the workspace's primary desktop toolchain) and the
// semantics are unaffected.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <doctest/doctest.h>

#include <ws/circsim/erc.hpp>
#include <ws/circsim/export.hpp>
#include <ws/circsim/library.hpp>
#include <ws/circsim/nets.hpp>

using ws::circsim::AcSpec;
using ws::circsim::Bounds;
using ws::circsim::DrawPrim;
using ws::circsim::Label;
using ws::circsim::OpSpec;
using ws::circsim::ParamEmit;
using ws::circsim::ParamSpec;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::SymbolDef;
using ws::circsim::SymbolLibrary;
using ws::circsim::Wire;

namespace {

const ws::circsim::Diag* find_diag(const std::vector<ws::circsim::Diag>& diags,
                                   std::string_view code) {
    for (const ws::circsim::Diag& d : diags) {
        if (d.code == code) {
            return &d;
        }
    }
    return nullptr;
}

bool has_diag(const std::vector<ws::circsim::Diag>& diags,
              std::string_view code) {
    return find_diag(diags, code) != nullptr;
}

// The first diag of `code` whose message mentions `needle` (null when
// none) - several checks share one code.
const ws::circsim::Diag* find_diag_mentioning(
    const std::vector<ws::circsim::Diag>& diags, std::string_view code,
    std::string_view needle) {
    for (const ws::circsim::Diag& d : diags) {
        if (d.code == code && d.message.find(needle) != std::string::npos) {
            return &d;
        }
    }
    return nullptr;
}

// Loads expecting success; REQUIREs so a hard failure stops the case.
SymbolLibrary load_ok(std::string_view text,
                      std::vector<ws::circsim::Diag>& diags) {
    SymbolLibrary lib;
    const ws::core::Status st = lib.load_text(text, "test.csym", diags);
    REQUIRE(st.has_value());
    return lib;
}

// The shipped tline example, inlined (tests run from the build tree).
constexpr std::string_view kTlineLib = R"json({
    "format": "circsim-symbols",
    "version": 1,
    "symbols": [
        {
            "key": "tline",
            "name": "Lossless transmission line",
            "spice": "T",
            "ref": "T",
            "pins": [
                {"name": "a1", "pos": [-4.0, 1.0], "dir": 180},
                {"name": "a2", "pos": [-4.0, -1.0], "dir": 180},
                {"name": "b1", "pos": [4.0, 1.0], "dir": 0},
                {"name": "b2", "pos": [4.0, -1.0], "dir": 0}
            ],
            "value": false,
            "model": false,
            "emit": [
                {"param": "z0", "prefix": "Z0=", "optional": false},
                {"param": "td", "prefix": "TD=", "optional": false}
            ],
            "params": [
                {"key": "z0", "type": "number", "label": "Z0 (ohms)", "default": "50"},
                {"key": "td", "type": "number", "label": "TD (seconds)", "default": "1n"}
            ],
            "draw": [
                {"line": [[-4.0, 1.0], [-1.0, 1.0]]},
                {"line": [[-4.0, -1.0], [-1.0, -1.0]]},
                {"line": [[1.0, 1.0], [4.0, 1.0]]},
                {"line": [[1.0, -1.0], [4.0, -1.0]]},
                {"rect": [-1.0, -2.0, 1.0, 2.0]},
                {"line": [[-1.0, 2.0], [1.0, -2.0]]}
            ],
            "vendor": "acme"
        }
    ],
    "author": "nobody"
}
)json";

// The quarter-wave demo schematic: V1 -- Rs -- T1, far port open, the
// near port's return and the far port's return grounded (the example
// deck's geometry).
Schematic tline_schematic() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "1", {0.0, -6.0}));
    v->params["ac"] = "1";
    Part* rs = s.add_part(Part::make("r", "Rs", "50", {8.0, 0.0}));
    Part* t = s.add_part(Part::make("tline", "T1", "", {15.0, -2.0}));
    t->params["z0"] = "50";
    t->params["td"] = "1u";
    s.add_part(Part::make("gnd", "", "", {3.0, -6.0}));  // v.n moved to +3 (stage 17d)
    s.add_part(Part::make("gnd", "", "", {11.0, -3.0}));
    s.add_part(Part::make("gnd", "", "", {19.0, -3.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0},
                                            PinRef{rs->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};  // v.p moved to -3 (stage 17d)
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{rs->id, 1},
                                            PinRef{t->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{2.0, 0.0};
    in->name = "in";
    Label* a = s.add_label({});
    a->pos = Pos{11.0, -0.5};
    a->name = "a";
    Label* b = s.add_label({});
    b->pos = Pos{19.0, -1.0};
    b->name = "b";
    AcSpec ac;
    ac.sweep = AcSpec::Sweep::Lin;
    ac.n = 3;
    ac.fstart = 250000.0;
    ac.fstop = 750000.0;
    s.analysis.entries.push_back(ac);
    return s;
}

}  // namespace

// --- the built-ins: specs, draw, geometry -------------------------------------

TEST_CASE("built-in symbols carry stage-2 specs and draw") {
    const SymbolDef* nmos = ws::circsim::find_symbol("nmos");
    REQUIRE(nmos != nullptr);
    REQUIRE(nmos->params.size() == 3);
    CHECK(nmos->params[0].key == "model");
    CHECK(nmos->params[0].type == ParamSpec::Type::Text);
    CHECK(nmos->params[1].key == "l");
    CHECK(nmos->params[1].type == ParamSpec::Type::Number);
    CHECK(nmos->params[2].key == "w");
    CHECK(nmos->params[2].type == ParamSpec::Type::Number);
    CHECK(!nmos->draw.empty());

    const SymbolDef* v = ws::circsim::find_symbol("v");
    REQUIRE(v != nullptr);
    REQUIRE(v->params.size() == 2);
    CHECK(v->params[0].key == "ac");
    CHECK(v->params[0].type == ParamSpec::Type::Text);
    CHECK(v->params[1].key == "wave");

    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    CHECK(r->params.empty());
    REQUIRE(r->draw.size() == 1);
    CHECK(r->draw[0].kind == DrawPrim::Kind::Poly);
    CHECK(r->draw[0].pts.size() == 7);

    const SymbolDef* gnd = ws::circsim::find_symbol("gnd");
    REQUIRE(gnd != nullptr);
    CHECK(gnd->draw.size() == 4);
    // Stage 17e: the ground hangs BELOW its pin and the pin points UP
    // into the circuit (the stage-2 table said 270 - into its own
    // bars, so a wire leaving the pin ran through the symbol).
    REQUIRE(gnd->pins.size() == 1);
    CHECK(gnd->pins[0].dir == 90);
    for (const DrawPrim& d : gnd->draw) {
        for (const Pos& p : d.pts) {
            CHECK(p.y <= 0.0);
        }
    }

    // The supply faces UP: the bar and label above the node, the pin
    // pointing down into the circuit below it.
    const SymbolDef* vcc = ws::circsim::find_symbol("vcc");
    REQUIRE(vcc != nullptr);
    REQUIRE(vcc->pins.size() == 1);
    CHECK(vcc->pins[0].dir == 270);
    REQUIRE(vcc->draw.size() == 3);
    bool bar_above = false;
    for (const DrawPrim& d : vcc->draw) {
        if (d.kind == DrawPrim::Kind::Line && !d.pts.empty() &&
            d.pts[0].y > 0.0 && d.pts[1].y > 0.0) {
            bar_above = true;
        }
    }
    CHECK(bar_above);
    bool label_above = false;
    for (const DrawPrim& d : vcc->draw) {
        if (d.kind == DrawPrim::Kind::Text && d.pts[0].y > 1.0) {
            label_above = true;
        }
    }
    CHECK(label_above);
}

TEST_CASE("symbol_bounds covers pins and primitives") {
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    const Bounds rb = ws::circsim::symbol_bounds(*r);
    CHECK(rb == Bounds{-3.0, -0.6, 3.0, 0.6});

    const SymbolDef* l = ws::circsim::find_symbol("l");
    REQUIRE(l != nullptr);
    const Bounds lb = ws::circsim::symbol_bounds(*l);
    CHECK(lb == Bounds{-3.0, -0.75, 3.0, 0.75});  // arcs bound by full circles

    const SymbolDef* v = ws::circsim::find_symbol("v");
    REQUIRE(v != nullptr);
    const Bounds vb = ws::circsim::symbol_bounds(*v);
    CHECK(vb == Bounds{-3.0, -1.0, 3.0, 1.0});  // stage 17d: pins/leads at +-3

    const SymbolDef* d = ws::circsim::find_symbol("d");
    REQUIRE(d != nullptr);
    const Bounds db = ws::circsim::symbol_bounds(*d);
    CHECK(db == Bounds{-3.0, -1.0, 3.0, 1.0});

    SymbolDef empty;
    empty.key = "empty";
    CHECK(ws::circsim::symbol_bounds(empty) == Bounds{0.0, 0.0, 0.0, 0.0});
}

TEST_CASE("transform_pos mirrors before rotating") {
    Part p;
    p.pos = {10.0, 10.0};
    p.mirror = false;
    p.rot = 0;
    CHECK(ws::circsim::transform_pos(p, Pos{2.0, 1.0}) == Pos{12.0, 11.0});
    p.mirror = true;
    CHECK(ws::circsim::transform_pos(p, Pos{2.0, 1.0}) == Pos{8.0, 11.0});
    p.mirror = false;
    p.rot = 90;
    CHECK(ws::circsim::transform_pos(p, Pos{2.0, 1.0}) == Pos{9.0, 12.0});
    p.rot = 180;
    CHECK(ws::circsim::transform_pos(p, Pos{2.0, 1.0}) == Pos{8.0, 9.0});
    p.rot = 270;
    CHECK(ws::circsim::transform_pos(p, Pos{2.0, 1.0}) == Pos{11.0, 8.0});
    // The pin path and the primitive path are the same transform.
    p.rot = 90;
    p.mirror = true;
    const SymbolDef* two = ws::circsim::find_symbol("r");
    REQUIRE(two != nullptr);
    p.pos = {0.0, 0.0};
    CHECK(ws::circsim::pin_pos(*two, p, 0) ==
          ws::circsim::transform_pos(p, Pos{two->pins[0].dx, two->pins[0].dy}));
}

// --- the library object ---------------------------------------------------------

TEST_CASE("a fresh library is the built-in table") {
    SymbolLibrary lib;
    CHECK(lib.symbols().size() == ws::circsim::builtin_symbols().size());
    CHECK(lib.builtin_count() == lib.symbols().size());
    CHECK(lib.find("R") != nullptr);   // case-insensitive
    CHECK(lib.find("NMOS") != nullptr);
    CHECK(lib.find("nope") == nullptr);
    CHECK(ws::circsim::builtin_library().find("gnd") != nullptr);
}

// --- .csym load -------------------------------------------------------------------

TEST_CASE("a good .csym loads every field") {
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(kTlineLib, diags);
    CHECK(diags.empty());

    const SymbolDef* t = lib.find("tline");
    REQUIRE(t != nullptr);
    CHECK(t->name == "Lossless transmission line");
    CHECK(t->spice_letter == 'T');
    CHECK(t->ref_prefix == "T");
    REQUIRE(t->pins.size() == 4);
    CHECK(t->pins[0].name == "a1");
    CHECK(t->pins[0].dx == -4.0);
    CHECK(t->pins[0].dy == 1.0);
    CHECK(t->pins[0].dir == 180);
    CHECK(t->pins[3].name == "b2");
    CHECK(!t->fixed_net.has_value());
    CHECK(t->needs_value == false);
    CHECK(t->needs_model == false);
    REQUIRE(t->emit.size() == 2);
    CHECK(t->emit[0].key == "z0");
    CHECK(t->emit[0].prefix == "Z0=");
    CHECK(t->emit[0].optional == false);
    REQUIRE(t->params.size() == 2);
    CHECK(t->params[0].type == ParamSpec::Type::Number);
    CHECK(t->params[0].label == "Z0 (ohms)");
    CHECK(t->params[0].default_value == "50");
    REQUIRE(t->draw.size() == 6);
    CHECK(t->draw[0].kind == DrawPrim::Kind::Line);
    CHECK(t->draw[4].kind == DrawPrim::Kind::Rect);
    // Unknown fields ride: symbol level and root level.
    CHECK(t->unknown.find("vendor") != nullptr);
    CHECK(lib.unknown.find("author") != nullptr);
    // One library symbol on top of the built-ins.
    CHECK(lib.symbols().size() == ws::circsim::builtin_symbols().size() + 1);
}

TEST_CASE("every draw primitive kind parses") {
    constexpr std::string_view text = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "k", "spice": "K",
            "draw": [
                {"line": [[0.0, 0.0], [1.0, 1.0]]},
                {"poly": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]]},
                {"polyfill": [[0.0, 0.0], [1.0, 0.0], [0.5, 1.0]]},
                {"rect": [0.0, 0.0, 2.0, 2.0]},
                {"circle": [0.0, 0.0, 1.5]},
                {"arc": [0.0, 0.0, 1.5, 10.0, 170.0]},
                {"dot": [0.0, 0.0, 0.4]},
                {"text": [0.0, 0.0, "hello"]}
            ]}]
    })json";
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(text, diags);
    CHECK(diags.empty());
    const SymbolDef* k = lib.find("k");
    REQUIRE(k != nullptr);
    REQUIRE(k->draw.size() == 8);
    CHECK(k->draw[0].kind == DrawPrim::Kind::Line);
    CHECK(k->draw[1].kind == DrawPrim::Kind::Poly);
    CHECK(k->draw[1].pts.size() == 3);
    CHECK(k->draw[2].kind == DrawPrim::Kind::PolyFill);
    CHECK(k->draw[2].pts.size() == 3);
    CHECK(k->draw[3].kind == DrawPrim::Kind::Rect);
    CHECK(k->draw[4].kind == DrawPrim::Kind::Circle);
    CHECK(k->draw[4].r == 1.5);
    CHECK(k->draw[5].kind == DrawPrim::Kind::Arc);
    CHECK(k->draw[5].a0 == 10.0);
    CHECK(k->draw[5].a1 == 170.0);
    CHECK(k->draw[6].kind == DrawPrim::Kind::Dot);
    CHECK(k->draw[7].kind == DrawPrim::Kind::Text);
    CHECK(k->draw[7].text == "hello");
}

TEST_CASE("tolerant load: repairs and skips with diagnostics") {
    constexpr std::string_view text = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [
            {"name": "no key at all"},
            {"key": "pinless", "pins": [
                {"pos": [1.0, 2.0], "dir": 0},
                {"name": "ok", "pos": "nope", "dir": "x", "extra": 1}
            ]},
            {"key": "odd", "spice": "?", "pins": [{"name": "p"}],
             "emit": [{"prefix": "X="}],
             "params": [{"key": "n", "type": "weird", "default": 50,
                         "choices": ["a", 7]}],
             "draw": [{"blob": []}, {"line": [[0, 0]]},
                      {"line": [[0, 0], [1, 1]], "circle": [0, 0, 1]}]}
        ]
    })json";
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(text, diags);
    CHECK(lib.find("no key at all") == nullptr);
    CHECK(has_diag(diags, "circsim.symbols.bad_symbol"));

    const SymbolDef* p = lib.find("pinless");
    REQUIRE(p != nullptr);
    REQUIRE(p->pins.size() == 1);       // the nameless pin dropped
    CHECK(p->pins[0].name == "ok");
    CHECK(p->pins[0].dx == 0.0);        // 'pos' "nope" repaired to [0, 0]
    CHECK(p->pins[0].dy == 0.0);
    CHECK(p->pins[0].dir == 180);       // 'dir' "x" repaired to default
    const SymbolDef* o = lib.find("odd");
    REQUIRE(o != nullptr);
    REQUIRE(o->pins.size() == 1);       // bare pin: defaults all round
    CHECK(o->pins[0].name == "p");
    CHECK(o->pins[0].dx == 0.0);
    CHECK(o->pins[0].dir == 180);
    CHECK(o->spice_letter == 0);   // '?' is not a letter
    CHECK(o->emit.empty());        // emit without a param dropped
    REQUIRE(o->params.size() == 1);
    CHECK(o->params[0].type == ParamSpec::Type::Text);  // unknown type
    CHECK(o->params[0].default_value == "50");          // number -> text
    REQUIRE(o->params[0].choices.size() == 1);
    CHECK(o->params[0].choices[0] == "a");
    CHECK(o->draw.empty());        // unknown shape, bad arity, two shapes
    CHECK(has_diag(diags, "circsim.symbols.bad_field"));
    CHECK(has_diag(diags, "circsim.symbols.bad_draw"));
}

TEST_CASE("hard failures: not JSON, wrong format") {
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib;
    CHECK(!lib.load_text("not json {", "x.csym", diags).has_value());
    CHECK(!lib.load_text("{\"format\": \"circsim\"}", "x.csym", diags)
               .has_value());
    CHECK(!lib.load_text("[1, 2, 3]", "x.csym", diags).has_value());
    CHECK(diags.empty());  // hard failures do not also diagnose
}

TEST_CASE("versions: absent migrates, future warns") {
    constexpr std::string_view no_version =
        R"json({"format": "circsim-symbols",
                 "symbols": [{"key": "k"}]})json";
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(no_version, diags);
    CHECK(lib.find("k") != nullptr);
    CHECK(has_diag(diags, "circsim.symbols.migrated"));

    constexpr std::string_view future =
        R"json({"format": "circsim-symbols", "version": 2,
                 "symbols": [{"key": "k2"}]})json";
    std::vector<ws::circsim::Diag> diags2;
    const SymbolLibrary lib2 = load_ok(future, diags2);
    CHECK(lib2.find("k2") != nullptr);
    CHECK(has_diag(diags2, "circsim.symbols.future_version"));
}

// --- layering ---------------------------------------------------------------------

TEST_CASE("redefinitions override, later files win") {
    constexpr std::string_view wide = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "r", "name": "Wide resistor",
                     "pins": [{"name": "a", "pos": [-4.0, 0.0]},
                              {"name": "b", "pos": [4.0, 0.0]}]}]
    })json";
    constexpr std::string_view wider = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "R", "name": "Wider resistor",
                     "pins": [{"name": "a", "pos": [-5.0, 0.0]},
                              {"name": "b", "pos": [5.0, 0.0]}]}]
    })json";
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib;
    REQUIRE(lib.load_text(wide, "wide.csym", diags).has_value());
    const SymbolDef* r = lib.find("r");
    REQUIRE(r != nullptr);
    CHECK(r->pins[0].dx == -4.0);
    CHECK(lib.symbols().size() == ws::circsim::builtin_symbols().size());
    CHECK(has_diag(diags, "circsim.symbols.override"));

    std::vector<ws::circsim::Diag> diags2;
    REQUIRE(lib.load_text(wider, "wider.csym", diags2).has_value());
    r = lib.find("r");
    REQUIRE(r != nullptr);
    CHECK(r->name == "Wider resistor");
    CHECK(r->pins[0].dx == -5.0);  // the later file won
    CHECK(lib.symbols().size() == ws::circsim::builtin_symbols().size());

    // A duplicate within one file: the second definition wins too.
    constexpr std::string_view dup = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "x", "name": "first"},
                    {"key": "x", "name": "second"}]
    })json";
    std::vector<ws::circsim::Diag> diags3;
    SymbolLibrary lib2;
    REQUIRE(lib2.load_text(dup, "dup.csym", diags3).has_value());
    CHECK(lib2.find("x")->name == "second");
    CHECK(has_diag(diags3, "circsim.symbols.override"));
}

TEST_CASE("save round-trips a library and its unknowns") {
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib = load_ok(kTlineLib, diags);
    const std::string once = lib.save();
    CHECK(once.find("\"tline\"") != std::string::npos);
    CHECK(once.find("\"vendor\"") != std::string::npos);   // symbol unknowns
    CHECK(once.find("\"author\"") != std::string::npos);   // root unknowns
    CHECK(once.find("\"r\"") == std::string::npos);  // built-ins stay out

    std::vector<ws::circsim::Diag> diags2;
    const SymbolLibrary again = load_ok(once, diags2);
    CHECK(diags2.empty());
    CHECK(again.save() == once);  // stable: save -> load -> save is a fixpoint
    const SymbolDef* t = again.find("tline");
    REQUIRE(t != nullptr);
    CHECK(t->unknown.find("vendor") != nullptr);

    // The full-table dump keeps the built-ins (and the override, were
    // there one).
    const std::string full = again.save(true);
    CHECK(full.find("\"r\"") != std::string::npos);
    CHECK(full.find("\"tline\"") != std::string::npos);
}

TEST_CASE("save keeps a built-in override on the user side of the cut") {
    constexpr std::string_view wide = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "r", "name": "Wide resistor",
                     "pins": [{"name": "a", "pos": [-4.0, 0.0]},
                              {"name": "b", "pos": [4.0, 0.0]}]}]
    })json";
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib = load_ok(wide, diags);
    const std::string text = lib.save();  // include_builtin = false
    std::vector<ws::circsim::Diag> diags2;
    const SymbolLibrary reloaded = load_ok(text, diags2);
    const SymbolDef* r = reloaded.find("r");
    REQUIRE(r != nullptr);
    CHECK(r->name == "Wide resistor");  // the override survived the round trip
    CHECK(r->pins[0].dx == -4.0);
}

// --- the param specs feed ERC -------------------------------------------------------

TEST_CASE("ERC checks number params, choices, unused and required") {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -6.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {1.0, -6.0}));
    Part* r = s.add_part(Part::make("r", "R1", "1k", {6.0, 0.0}));
    r->params["foo"] = "1";  // r specs and emits nothing: unused
    Part* m = s.add_part(Part::make("nmos", "M1", "", {14.0, 0.0}));
    m->params["model"] = "nch";
    m->params["l"] = "wide";  // not a SPICE number
    m->params["w"] = "2u";
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r->id, 1}, PinRef{m->id, 2}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 1}, PinRef{g->id, 0}));

    const std::vector<ws::circsim::Diag> diags = ws::circsim::run_erc(s);
    const ws::circsim::Diag* bad = find_diag(diags, "circsim.erc.bad_param");
    REQUIRE(bad != nullptr);              // l = "wide" is not a number
    CHECK(bad->message.find("wide") != std::string::npos);
    CHECK(bad->severity == ws::circsim::Severity::Error);
    CHECK(has_diag(diags, "circsim.erc.unused_param"));    // foo on r

    // A choice param outside its list warns; inside (ci) passes.
    constexpr std::string_view lib_text = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "pick", "spice": "R",
                     "params": [{"key": "mode", "type": "choice",
                                 "choices": ["fast", "slow"]}]}]
    })json";
    std::vector<ws::circsim::Diag> ldiags;
    SymbolLibrary lib = load_ok(lib_text, ldiags);
    Part* pick = s.add_part(Part::make("pick", "P1", "1", {22.0, 0.0}));
    pick->params["mode"] = "FAST";  // ci match: fine
    std::vector<ws::circsim::Diag> d2 = ws::circsim::run_erc(s, &lib);
    CHECK(find_diag_mentioning(d2, "circsim.erc.bad_param", "mode") ==
          nullptr);
    pick->params["mode"] = "sideways";
    d2 = ws::circsim::run_erc(s, &lib);
    const ws::circsim::Diag* choice = find_diag_mentioning(
        d2, "circsim.erc.bad_param", "sideways");
    REQUIRE(choice != nullptr);
    CHECK(choice->severity == ws::circsim::Severity::Warning);
    CHECK(choice->message.find("fast") != std::string::npos);  // lists them
}

TEST_CASE("a required emit entry is a missing-param error") {
    std::vector<ws::circsim::Diag> ldiags;
    const SymbolLibrary lib = load_ok(kTlineLib, ldiags);
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "1", {0.0, -6.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {1.0, -6.0}));
    Part* t = s.add_part(Part::make("tline", "T1", "", {8.0, 0.0}));
    t->params["z0"] = "50";  // td missing -> the card cannot form
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{t->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 1}, PinRef{g->id, 0}));
    const std::vector<ws::circsim::Diag> diags = ws::circsim::run_erc(s, &lib);
    const ws::circsim::Diag* miss = find_diag(diags, "circsim.erc.missing_param");
    REQUIRE(miss != nullptr);
    CHECK(miss->message.find("td") != std::string::npos);
    CHECK(miss->severity == ws::circsim::Severity::Error);
}

TEST_CASE("default_params is the new-part prefill, never the export") {
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(kTlineLib, diags);
    const SymbolDef* t = lib.find("tline");
    REQUIRE(t != nullptr);
    const std::map<std::string, std::string> defs =
        ws::circsim::default_params(*t);
    REQUIRE(defs.size() == 2);
    CHECK(defs.at("z0") == "50");
    CHECK(defs.at("td") == "1n");

    // A part that carries NOTHING exports nothing for z0/td: the file is
    // the truth, defaults are a prefill only.
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "1", {0.0, -6.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {1.0, -6.0}));
    Part* t2 = s.add_part(Part::make("tline", "T1", "", {8.0, 0.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{t2->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 1}, PinRef{g->id, 0}));
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s, &lib);
    const auto deck =
        ws::circsim::export_deck(s, nets.nets, nullptr, nullptr, &lib);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find("Z0=") == std::string::npos);
}

// --- the full path through a library symbol --------------------------------------

TEST_CASE("a library symbol exports and simulates") {
    std::vector<ws::circsim::Diag> ldiags;
    const SymbolLibrary lib = load_ok(kTlineLib, ldiags);

    const Schematic s = tline_schematic();
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s, &lib);
    CHECK(nets.diags.empty());

    // The tline's four pins land where the geometry says.
    const SymbolDef* t = lib.find("tline");
    REQUIRE(t != nullptr);
    const Part* t1 = nullptr;
    for (const Part& p : s.parts()) {
        if (p.symbol == "tline") {
            t1 = &p;
        }
    }
    REQUIRE(t1 != nullptr);
    CHECK(ws::circsim::pin_pos(*t, *t1, 0) == Pos{11.0, -1.0});
    CHECK(ws::circsim::pin_pos(*t, *t1, 3) == Pos{19.0, -3.0});
    const ws::circsim::Net* na = nets.nets.by_name("a");
    REQUIRE(na != nullptr);
    CHECK(na->pins.size() == 2);  // Rs.b + T1.a1

    const auto deck =
        ws::circsim::export_deck(s, nets.nets, nullptr, nullptr, &lib);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find("T1 a 0 b 0 Z0=50 TD=1u") != std::string::npos);
    CHECK(deck->text.find("Rs in a 50") != std::string::npos);
    CHECK(deck->text.find("V1 in 0 1 AC 1") != std::string::npos);

    // Through ws::spice's own front end: the quarter-wave answers.
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s, nullptr,
                                                                    {}, &lib);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& tbl = out.run->results.tables.front();
    CHECK(tbl.row_count() == 3);
    std::size_t va = tbl.signals.size();
    for (std::size_t i = 0; i < tbl.signals.size(); ++i) {
        if (tbl.signals[i].name == "re(v(a))") {
            va = i;
        }
    }
    REQUIRE(va < tbl.signals.size());
    // row() columns are the sweep axes then the signals (the CSV shape).
    const std::size_t col = va + tbl.sweep_names.size();
    // 250k: the input looks short (v(a) ~ 0); 500k: open (v(a) ~ 1);
    // 750k: short again.
    CHECK(std::abs(tbl.row(0)[col]) < 0.05);
    CHECK(std::abs(tbl.row(1)[col] - 1.0) < 0.05);
    CHECK(std::abs(tbl.row(2)[col]) < 0.05);
}

TEST_CASE("an override changes the coincidence-built nets") {
    constexpr std::string_view wide = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "r", "name": "Wide resistor",
                     "pins": [{"name": "a", "pos": [-4.0, 0.0]},
                              {"name": "b", "pos": [4.0, 0.0]}]}]
    })json";
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(wide, diags);

    // Two resistors touching PIN TO PIN at (9, 0) - no wire between them:
    // with the built-ins the exact-position coincidence unions them; with
    // the wide override the pins move to 8 and 10 and the touch is gone.
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "10k", {6.0, 0.0}));
    Part* r2 = s.add_part(Part::make("r", "R2", "10k", {12.0, 0.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r1->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r2->id, 1}, PinRef{g->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    Label* out = s.add_label({});
    out->pos = Pos{9.0, 0.0};
    out->name = "out";

    const ws::circsim::NetBuild plain = ws::circsim::build_nets(s);
    CHECK(plain.diags.empty());
    const ws::circsim::Net* n_out = plain.nets.by_name("out");
    REQUIRE(n_out != nullptr);
    CHECK(n_out->pins.size() == 2);  // r1.b + r2.a touch

    const ws::circsim::NetBuild wide_nets = ws::circsim::build_nets(s, &lib);
    CHECK(wide_nets.nets.by_name("out") == nullptr);  // the touch is gone
    CHECK(has_diag(wide_nets.diags, "circsim.nets.label_unattached"));
}
