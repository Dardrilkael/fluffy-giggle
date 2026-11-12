// The stage-2 symbol library: .csym load/save, layering (built-ins +
// overrides), the typed param specs, the draw primitives, and the ERC
// checks they enable - plus the full schematic -> deck -> run path
// through a library-defined symbol.

#include <cmath>
#include <map>
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
    // The even-slope zigzag since 50bffe3: 10 points (leads + teeth).
    CHECK(r->draw[0].pts.size() == 10);

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

TEST_CASE("the LED is a diode the editor can light") {
    // The built-in LED: the diode's SPICE identity (letter D, model +
    // area, ref Dn) plus the display-only `color` Choice the canvas glow
    // reads - spec'd so ERC never flags it, absent from the emit list so
    // the deck stays a plain diode card.
    const SymbolDef* led = ws::circsim::find_symbol("led");
    REQUIRE(led != nullptr);
    CHECK(led->spice_letter == 'D');
    CHECK(led->ref_prefix == "D");
    CHECK(led->needs_model);
    CHECK(!led->needs_value);
    REQUIRE(led->pins.size() == 2);
    CHECK(led->pins[0].name == "a");
    CHECK(led->pins[1].name == "k");

    // The params: model, area, then the color choice with its list.
    REQUIRE(led->params.size() == 3);
    CHECK(led->params[0].key == "model");
    CHECK(led->params[2].key == "color");
    CHECK(led->params[2].type == ParamSpec::Type::Choice);
    CHECK(led->params[2].default_value == "red");
    REQUIRE(led->params[2].choices.size() == 6);
    CHECK(led->params[2].choices[0] == "red");
    CHECK(led->params[2].choices[5] == "orange");

    // The emit tail carries model + area only - the color never decks.
    REQUIRE(led->emit.size() == 2);
    CHECK(led->emit[0].key == "model");
    CHECK(led->emit[1].key == "area");

    // The picture: the diode body plus two filled emission arrowheads.
    std::size_t fills = 0;
    for (const DrawPrim& d : led->draw) {
        if (d.kind == DrawPrim::Kind::PolyFill) {
            ++fills;
        }
    }
    CHECK(fills == 2);
    const Bounds b = ws::circsim::symbol_bounds(*led);
    CHECK(b == Bounds{-3.0, -1.0, 3.0, 1.5});  // the arrows reach past the body

    // default_params prefills the color; a NEW part carries it without
    // ever reaching the deck.
    const std::map<std::string, std::string> dp =
        ws::circsim::default_params(*led);
    CHECK(dp.at("color") == "red");
}

TEST_CASE("symbol_bounds covers pins and primitives") {
    const SymbolDef* r = ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);
    const Bounds rb = ws::circsim::symbol_bounds(*r);
    // The 0.7-high zigzag teeth bound y since 50bffe3 (was 0.6).
    CHECK(rb == Bounds{-3.0, -0.7, 3.0, 0.7});
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
    // tline is a built-in since stage 24: loading the example now
    // overrides it in place (warned), not appended.
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == "circsim.symbols.override");

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
    // The override keeps the built-in's slot: same table size.
    CHECK(lib.symbols().size() == ws::circsim::builtin_symbols().size());
    CHECK(lib.builtin_count() ==
          ws::circsim::builtin_symbols().size());
}

TEST_CASE("every draw primitive kind parses") {
    constexpr std::string_view text = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "prims", "spice": "K",
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
    const SymbolDef* k = lib.find("prims");
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
                 "symbols": [{"key": "ver1"}]})json";
    std::vector<ws::circsim::Diag> diags;
    const SymbolLibrary lib = load_ok(no_version, diags);
    CHECK(lib.find("ver1") != nullptr);
    CHECK(has_diag(diags, "circsim.symbols.migrated"));

    constexpr std::string_view future =
        R"json({"format": "circsim-symbols", "version": 2,
                 "symbols": [{"key": "ver2"}]})json";
    std::vector<ws::circsim::Diag> diags2;
    const SymbolLibrary lib2 = load_ok(future, diags2);
    CHECK(lib2.find("ver2") != nullptr);
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
    // The saved text still carries the tline override of the built-in,
    // so the load warns again - the fixpoint below is unaffected.
    REQUIRE(diags2.size() == 1);
    CHECK(diags2[0].code == "circsim.symbols.override");
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

TEST_CASE("the blade rides a .csym round trip, and a broken one drops") {
    // A .csym switch with a blade: load, read, save, reload - the
    // geometry must survive byte-stable (the fixpoint the other fields
    // hold). The built-in s override warns, as overrides do.
    constexpr std::string_view kBladeLib = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{
            "key": "s", "name": "Switch (voltage controlled)",
            "spice": "S", "ref": "S", "model": true, "value": false,
            "model_kind": "sw",
            "pins": [
                {"name": "p", "pos": [-5.0, 0.0], "dir": 180},
                {"name": "n", "pos": [5.0, 0.0], "dir": 0},
                {"name": "c+", "pos": [2.0, -4.0], "dir": 270},
                {"name": "c-", "pos": [-2.0, -4.0], "dir": 270}
            ],
            "emit": [{"param": "model", "optional": false}],
            "params": [{"key": "model", "label": "Model"}],
            "draw": [
                {"rect": [-2.5, -2.0, 2.5, 2.0]},
                {"line": [[-5.0, 0.0], [-1.2, 0.0]]},
                {"dot": [-1.2, 0.0, 0.14]},
                {"dot": [1.2, 0.0, 0.14]},
                {"line": [[1.2, 0.0], [5.0, 0.0]]}
            ],
            "blade": {
                "pivot": [-1.2, 0.0],
                "open": [0.9, 0.95],
                "closed": [1.2, 0.0]
            }
        }]
    })json";
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib = load_ok(kBladeLib, diags);
    const SymbolDef* s = lib.find("s");
    REQUIRE(s != nullptr);
    REQUIRE(s->blade.has_value());
    CHECK(s->blade->pivot.x == doctest::Approx(-1.2));
    CHECK(s->blade->open_tip.y == doctest::Approx(0.95));
    CHECK(s->blade->closed_tip.x == doctest::Approx(1.2));
    // Save keeps the blade (an override of a built-in stays on the
    // user side of the cut), and the reload reads the same geometry.
    const std::string once = lib.save();
    CHECK(once.find("\"blade\"") != std::string::npos);
    std::vector<ws::circsim::Diag> diags2;
    const SymbolLibrary again = load_ok(once, diags2);
    const SymbolDef* s2 = again.find("s");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2->blade.has_value());
    CHECK(s2->blade->pivot.x == doctest::Approx(-1.2));
    CHECK(s2->blade->open_tip.y == doctest::Approx(0.95));
    CHECK(s2->blade->closed_tip.x == doctest::Approx(1.2));
    CHECK(again.save() == once);

    // The built-ins' own blades ride the full dump too: the s/w pair
    // survive a dump -> reload without losing their moving contact.
    SymbolLibrary fresh;
    std::vector<ws::circsim::Diag> diags3;
    const SymbolLibrary full = load_ok(fresh.save(true), diags3);
    const SymbolDef* sb = full.find("s");
    REQUIRE(sb != nullptr);
    REQUIRE(sb->blade.has_value());
    CHECK(sb->blade->open_tip.y == doctest::Approx(0.95));
    const SymbolDef* wb = full.find("w");
    REQUIRE(wb != nullptr);
    REQUIRE(wb->blade.has_value());

    // A malformed blade (a missing endpoint) warns and drops to the
    // static picture - never a half-animated symbol.
    constexpr std::string_view kBadBladeLib = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{
            "key": "mytr", "name": "My throwover", "spice": "S",
            "ref": "S",
            "pins": [
                {"name": "p", "pos": [-3.0, 0.0], "dir": 180},
                {"name": "n", "pos": [3.0, 0.0], "dir": 0},
                {"name": "c+", "pos": [2.0, -3.0], "dir": 270},
                {"name": "c-", "pos": [-2.0, -3.0], "dir": 270}
            ],
            "blade": {"pivot": [-1.0, 0.0], "open": [1.0, 1.0]}
        }]
    })json";
    std::vector<ws::circsim::Diag> diags4;
    const SymbolLibrary bad = load_ok(kBadBladeLib, diags4);
    bool saw_blade_diag = false;
    for (const ws::circsim::Diag& d : diags4) {
        if (d.code == "circsim.symbols.bad_field" &&
            d.message.find("'blade'") != std::string::npos) {
            saw_blade_diag = true;
        }
    }
    CHECK(saw_blade_diag);
    const SymbolDef* mytr = bad.find("mytr");
    REQUIRE(mytr != nullptr);
    CHECK(!mytr->blade.has_value());
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

// --- the orphan devices join the built-ins (stage 24) ------------------------------

TEST_CASE("the orphan-device built-ins carry the SPICE letters and specs") {
    // The controlled sources: four nodes OUT+ OUT- IN+ IN-, the gain
    // as the value (E/G are the only new symbols that keep one).
    const SymbolDef* e = ws::circsim::find_symbol("e");
    REQUIRE(e != nullptr);
    CHECK(e->spice_letter == 'E');
    CHECK(e->ref_prefix == "E");
    REQUIRE(e->pins.size() == 4);
    CHECK(e->pins[0].name == "out+");
    CHECK(e->pins[1].name == "out-");
    CHECK(e->pins[2].name == "in+");
    CHECK(e->pins[3].name == "in-");
    CHECK(e->needs_value);
    const SymbolDef* g = ws::circsim::find_symbol("g");
    REQUIRE(g != nullptr);
    CHECK(g->spice_letter == 'G');
    REQUIRE(g->pins.size() == 4);
    CHECK(g->pins[0].name == e->pins[0].name);  // the same card order

    // The current-controlled pair: two pins, and the controlling V
    // source's name rides BEFORE the gain (the card grammar's order,
    // which forbids the value-first spelling).
    const SymbolDef* f = ws::circsim::find_symbol("f");
    REQUIRE(f != nullptr);
    CHECK(f->spice_letter == 'F');
    REQUIRE(f->pins.size() == 2);
    CHECK(!f->needs_value);
    REQUIRE(f->params.size() == 2);
    CHECK(f->params[0].key == "vs");
    CHECK(f->params[0].type == ParamSpec::Type::Text);
    CHECK(f->params[1].key == "gain");
    CHECK(f->params[1].type == ParamSpec::Type::Number);
    REQUIRE(f->emit.size() == 2);
    CHECK(!f->emit[0].optional);  // vs and gain are both required
    CHECK(!f->emit[1].optional);
    const SymbolDef* h = ws::circsim::find_symbol("h");
    REQUIRE(h != nullptr);
    CHECK(h->spice_letter == 'H');
    REQUIRE(h->emit.size() == 2);
    CHECK(h->emit[1].key == "r");

    // The behavioral source: exactly one of V=/I= is meant; both are
    // optional at the table level, the deck rejects the bad combos.
    const SymbolDef* b = ws::circsim::find_symbol("b");
    REQUIRE(b != nullptr);
    CHECK(b->spice_letter == 'B');
    CHECK(!b->needs_value);
    REQUIRE(b->emit.size() == 2);
    CHECK(b->emit[0].prefix == "V=");
    CHECK(b->emit[1].prefix == "I=");
    CHECK(b->emit[0].optional);
    CHECK(b->emit[1].optional);

    // The switches: model required, W names the controlling source.
    const SymbolDef* sw = ws::circsim::find_symbol("s");
    REQUIRE(sw != nullptr);
    CHECK(sw->spice_letter == 'S');
    CHECK(sw->needs_model);
    CHECK(!sw->needs_value);
    REQUIRE(sw->pins.size() == 4);
    CHECK(sw->pins[0].name == "p");   // the card spells p n c+ c-
    CHECK(sw->pins[2].name == "c+");
    const SymbolDef* w = ws::circsim::find_symbol("w");
    REQUIRE(w != nullptr);
    CHECK(w->spice_letter == 'W');
    CHECK(w->needs_model);
    REQUIRE(w->emit.size() == 2);
    CHECK(w->emit[0].key == "vs");
    CHECK(!w->emit[0].optional);

    // The blade (the animated contact): the pivot on the first contact
    // dot, the open tip at the end the old static line drew, the
    // closed tip on the mate contact - and the s/w pair are the only
    // symbols in the table that carry one.
    REQUIRE(sw->blade.has_value());
    CHECK(sw->blade->pivot.x == doctest::Approx(-1.2));
    CHECK(sw->blade->pivot.y == doctest::Approx(0.0));
    CHECK(sw->blade->open_tip.x == doctest::Approx(0.9));
    CHECK(sw->blade->open_tip.y == doctest::Approx(0.95));
    CHECK(sw->blade->closed_tip.x == doctest::Approx(1.2));
    CHECK(sw->blade->closed_tip.y == doctest::Approx(0.0));
    REQUIRE(w->blade.has_value());
    CHECK(w->blade->pivot.x == doctest::Approx(-0.9));
    CHECK(w->blade->pivot.y == doctest::Approx(0.0));
    CHECK(w->blade->open_tip.x == doctest::Approx(0.6));
    CHECK(w->blade->open_tip.y == doctest::Approx(0.8));
    CHECK(w->blade->closed_tip.x == doctest::Approx(0.9));
    CHECK(w->blade->closed_tip.y == doctest::Approx(0.0));
    for (const SymbolDef& s : ws::circsim::builtin_symbols()) {
        if (s.key != "s" && s.key != "w") {
            INFO("symbol key: ", s.key);
            CHECK(!s.blade.has_value());
        }
    }
    // And the bounds still hold the blade's swing (symbol_bounds grew
    // the endpoints): the s body's box spans the blade either way, so
    // the answer is the pins' and the box's own.
    const Bounds sb = symbol_bounds(*sw);
    CHECK(sb.min_x == doctest::Approx(-5.0));  // the p pin
    CHECK(sb.max_x == doctest::Approx(5.0));   // the n pin
    CHECK(sb.min_y == doctest::Approx(-4.0));  // the c pair's pins
    CHECK(sb.max_y == doctest::Approx(2.0));   // the body box's top


    // The coupling: deliberately pinless - it names inductors.
    const SymbolDef* k = ws::circsim::find_symbol("k");
    REQUIRE(k != nullptr);
    CHECK(k->spice_letter == 'K');
    CHECK(k->pins.empty());
    CHECK(!k->needs_value);
    REQUIRE(k->emit.size() == 3);
    CHECK(k->emit[0].key == "l1");
    CHECK(k->emit[1].key == "l2");
    CHECK(k->emit[2].key == "k");
    for (const ParamEmit& pe : k->emit) {
        CHECK(!pe.optional);
    }

    // The transformer: the multi-card shape. Three cards - Lp, Ls and
    // the K coupling that names its own siblings.
    const SymbolDef* xf = ws::circsim::find_symbol("xfmr");
    REQUIRE(xf != nullptr);
    CHECK(xf->spice_letter == 'K');
    CHECK(xf->ref_prefix == "K");
    CHECK(!xf->needs_value);
    REQUIRE(xf->pins.size() == 4);
    CHECK(xf->pins[0].name == "p+");
    CHECK(xf->pins[2].name == "s+");
    REQUIRE(xf->card_emits.size() == 3);
    CHECK(xf->card_emits[0].letter == 'L');
    CHECK(xf->card_emits[0].suffix == "p");
    CHECK(xf->card_emits[0].pins == std::vector<std::uint32_t>{0, 1});
    CHECK(xf->card_emits[0].emit[0].key == "lp");
    CHECK(!xf->card_emits[0].emit[0].optional);
    CHECK(xf->card_emits[1].letter == 'L');
    CHECK(xf->card_emits[1].suffix == "s");
    CHECK(xf->card_emits[1].pins == std::vector<std::uint32_t>{2, 3});
    CHECK(xf->card_emits[2].letter == 'K');
    CHECK(xf->card_emits[2].pins.empty());
    CHECK(xf->card_emits[2].name_siblings);  // the K names Lp and Ls
    CHECK(xf->card_emits[2].emit[0].key == "k");
    // A new part prefills usable inductances and coupling.
    const std::map<std::string, std::string> defs =
        ws::circsim::default_params(*xf);
    CHECK(defs.at("lp") == "1m");
    CHECK(defs.at("ls") == "1m");
    CHECK(defs.at("k") == "0.98");

    // The tline is a built-in now: same four pins, same Z0=/TD=.
    const SymbolDef* tl = ws::circsim::find_symbol("tline");
    REQUIRE(tl != nullptr);
    CHECK(tl->spice_letter == 'T');
    REQUIRE(tl->pins.size() == 4);
    CHECK(tl->pins[0].name == "in+");
    CHECK(tl->pins[0].dx == -4.0);
    CHECK(tl->pins[0].dy == 1.0);
    CHECK(!tl->needs_value);
    REQUIRE(tl->emit.size() == 2);
    CHECK(tl->emit[0].prefix == "Z0=");
    CHECK(!tl->emit[0].optional);
    CHECK(tl->emit[1].prefix == "TD=");
    CHECK(!tl->emit[1].optional);
}

TEST_CASE("the built-in tline exports and simulates without any library") {
    // The promoted symbol: the quarter-wave demo runs on the built-in
    // table alone - no -S, no .csym layer.
    const Schematic s = tline_schematic();
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    CHECK(nets.diags.empty());
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "Rs in a 50\n"
          "T1 a 0 b 0 Z0=50 TD=1u\n"
          "V1 in 0 1 AC 1\n"
          ".ac lin 3 250000 750000\n"
          ".end\n");

    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& tbl = out.run->results.tables.front();
    CHECK(tbl.row_count() == 3);
    const auto va = tbl.signal("re(v(a))");
    REQUIRE(va.size() == 3);
    CHECK(std::abs(va[0]) < 0.05);          // 250k: the line looks short
    CHECK(std::abs(va[1] - 1.0) < 0.05);    // 500k: open
    CHECK(std::abs(va[2]) < 0.05);          // 750k: short again
}

// --- the model library's shelf kinds (stage 25) -----------------------------

TEST_CASE("every model-bearing symbol declares its library kind") {
    // The kinds map 1:1 onto the .model TYPE tokens ("d", "npn", ...);
    // "opamp" is the one extension (a .subckt card). The property grid's
    // picker offers this kind's cards, ERC checks a named card against
    // it, export emits the card a part names.
    struct Row {
        const char* key;
        const char* kind;
    };
    for (const Row& row : {Row{"d", "d"}, Row{"led", "d"}, Row{"npn", "npn"},
                           Row{"pnp", "pnp"}, Row{"nmos", "nmos"},
                           Row{"pmos", "pmos"}, Row{"njf", "njf"},
                           Row{"pjf", "pjf"}, Row{"s", "sw"}, Row{"w", "sw"},
                           Row{"opamp", "opamp"}}) {
        const SymbolDef* sym = ws::circsim::find_symbol(row.key);
        REQUIRE(sym != nullptr);
        CHECK(sym->model_kind == row.kind);
        CHECK(sym->needs_model);
    }
    // A part that takes no model carries no kind: the text row is the
    // whole story for these.
    for (const char* key : {"r", "c", "l", "v", "i", "gnd", "vcc", "tline"}) {
        const SymbolDef* sym = ws::circsim::find_symbol(key);
        REQUIRE(sym != nullptr);
        CHECK(sym->model_kind.empty());
    }
}

TEST_CASE("the op amp is a five-pin X instance over the model library") {
    // One symbol, one `model` param - the subckt NAME it decks as
    // `X<ref> out inp inn vp vm <model>`. The built-in library's opamp
    // cards (linear macromodels) are what the picker offers; the pins
    // land at whole-grid points with leads pointing into the body.
    const SymbolDef* op = ws::circsim::find_symbol("opamp");
    REQUIRE(op != nullptr);
    CHECK(op->spice_letter == 'X');
    CHECK(op->ref_prefix == "X");
    CHECK(op->needs_model);
    CHECK(!op->needs_value);
    CHECK(op->model_kind == "opamp");
    REQUIRE(op->pins.size() == 5);
    CHECK(op->pins[0].name == "out");
    CHECK(op->pins[1].name == "in+");
    CHECK(op->pins[2].name == "in-");
    CHECK(op->pins[3].name == "v+");
    CHECK(op->pins[4].name == "v-");
    for (const ws::circsim::PinDef& p : op->pins) {
        // Whole grid units so rotated pins stay on grid points, and a
        // lead direction pointing INTO the body (the invariants' rule).
        CHECK(p.dx == std::floor(p.dx));
        CHECK(p.dy == std::floor(p.dy));
    }
    // The params: just the model.
    REQUIRE(op->params.size() == 1);
    CHECK(op->params[0].key == "model");
    CHECK(op->params[0].type == ParamSpec::Type::Text);
    REQUIRE(op->emit.size() == 1);
    CHECK(op->emit[0].key == "model");
    CHECK(!op->emit[0].optional);
    // The picture: a triangle, the +/- input marks, the supply stubs.
    CHECK(!op->draw.empty());
    bool triangle = false;
    for (const DrawPrim& d : op->draw) {
        if (d.kind == DrawPrim::Kind::Poly && d.pts.size() == 4) {
            triangle = true;
        }
    }
    CHECK(triangle);
}

TEST_CASE("a .csym symbol's model_kind round-trips") {
    // Stage 25: a .csym semiconductor names which library shelf its
    // picker offers and ERC checks against - the field must survive a
    // save -> load cycle untouched, and only when set (every classic
    // .csym stays byte-identical).
    constexpr std::string_view with_kind = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "myd", "name": "My diode", "spice": "D",
                     "ref": "D", "model_kind": "d",
                     "pins": [{"name": "a", "pos": [-3.0, 0.0], "dir": 180},
                              {"name": "k", "pos": [3.0, 0.0], "dir": 0}]}]
    })json";
    std::vector<ws::circsim::Diag> diags;
    SymbolLibrary lib;
    REQUIRE(lib.load_text(with_kind, "kind.csym", diags).has_value());
    const SymbolDef* d = lib.find("myd");
    REQUIRE(d != nullptr);
    CHECK(d->model_kind == "d");

    const std::string saved = lib.save();
    CHECK(saved.find("\"model_kind\": \"d\"") != std::string::npos);
    std::vector<ws::circsim::Diag> diags2;
    SymbolLibrary again;
    REQUIRE(again.load_text(saved, "kind2.csym", diags2).has_value());
    REQUIRE(again.find("myd") != nullptr);
    CHECK(again.find("myd")->model_kind == "d");
    CHECK(again.save() == saved);  // the fixpoint holds

    // A kind the library has not met yet still loads (it rides as-is -
    // the built-in table grows kinds the editor has not met), and a
    // non-text value warns and lands empty.
    constexpr std::string_view odd = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "zed", "name": "Zed", "spice": "Z", "ref": "Z",
                     "model_kind": "zed",
                     "pins": [{"name": "a", "pos": [-2.0, 0.0], "dir": 180},
                              {"name": "b", "pos": [2.0, 0.0], "dir": 0}]}]
    })json";
    std::vector<ws::circsim::Diag> diags3;
    REQUIRE(lib.load_text(odd, "odd.csym", diags3).has_value());
    REQUIRE(lib.find("zed") != nullptr);
    CHECK(lib.find("zed")->model_kind == "zed");

    constexpr std::string_view bad = R"json({
        "format": "circsim-symbols", "version": 1,
        "symbols": [{"key": "bad", "name": "Bad", "spice": "Z", "ref": "Z",
                     "model_kind": 7,
                     "pins": [{"name": "a", "pos": [-2.0, 0.0], "dir": 180},
                              {"name": "b", "pos": [2.0, 0.0], "dir": 0}]}]
    })json";
    std::vector<ws::circsim::Diag> diags4;
    REQUIRE(lib.load_text(bad, "bad.csym", diags4).has_value());
    REQUIRE(lib.find("bad") != nullptr);
    CHECK(lib.find("bad")->model_kind.empty());
    CHECK(has_diag(diags4, "circsim.symbols.bad_field"));
}
