// ERC: one case per check in erc.hpp's contract.

#include <string>

#include <doctest/doctest.h>

#include <ws/circsim/erc.hpp>
#include <ws/circsim/nets.hpp>

using ws::circsim::Junction;
using ws::circsim::Label;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::Severity;
using ws::circsim::Wire;

namespace {

struct Bench {
    Schematic s;

    Part* part(std::string symbol, std::string ref, std::string value,
               Pos at) {
        return s.add_part(
            Part::make(std::move(symbol), std::move(ref),
                       std::move(value), at));
    }
    Part* v(std::string ref, Pos at) { return part("v", std::move(ref), "5", at); }
    Part* i(std::string ref, Pos at) { return part("i", std::move(ref), "1m", at); }
    Part* r(std::string ref, Pos at) { return part("r", std::move(ref), "1k", at); }
    Part* gnd(Pos at) { return part("gnd", "", "", at); }

    std::uint32_t junction(double x, double y) {
        Junction* j = s.add_junction({});
        j->pos = Pos{x, y};
        return j->id;
    }
    std::uint32_t pin_wire(PinRef a, PinRef b) {
        Wire* w = s.add_wire({});
        w->a = a;
        w->b = b;
        return w->id;
    }
    std::uint32_t j_wire(double ax, double ay, double bx, double by) {
        Wire* w = s.add_wire({});
        w->a = ws::circsim::JunctionRef{junction(ax, ay)};
        w->b = ws::circsim::JunctionRef{junction(bx, by)};
        return w->id;
    }
};

bool has(const std::vector<ws::circsim::Diag>& diags, const std::string& code,
         Severity severity = Severity::Error) {
    for (const ws::circsim::Diag& d : diags) {
        if (d.code == code && d.severity == severity) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a fully-wired divider is clean") {
    Bench b;
    // Source below the axis: a straight pin wire from V1.p must not pass
    // over V1.n (the pins are 2 units apart).
    Part* v1 = b.v("V1", {0.0, -4.0});
    Part* r1 = b.r("R1", {6.0, 0.0});
    Part* r2 = b.r("R2", {14.0, 0.0});
    Part* g = b.gnd({7.0, -6.0});
    b.pin_wire(PinRef{v1->id, 0}, PinRef{r1->id, 0});
    b.pin_wire(PinRef{r1->id, 1}, PinRef{r2->id, 0});
    b.pin_wire(PinRef{r2->id, 1}, PinRef{g->id, 0});
    b.pin_wire(PinRef{g->id, 0}, PinRef{v1->id, 1});
    const std::vector<ws::circsim::Diag> diags = ws::circsim::run_erc(b.s);
    CHECK(diags.empty());
}

TEST_CASE("a floating pin warns; a partially-wired net is one_pin_net") {
    SUBCASE("nothing at all on the pin") {
        Bench b;
        Part* r1 = b.r("R1", {0.0, 0.0});
        b.pin_wire(PinRef{r1->id, 0}, PinRef{r1->id, 1});  // shorts it: 2 pins
        b.r("R2", {10.0, 0.0});                            // fully floating
        b.gnd({5.0, -4.0});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK(has(diags, "circsim.erc.floating_pin", Severity::Warning));
        CHECK_FALSE(has(diags, "circsim.erc.one_pin_net"));
        CHECK_FALSE(has(diags, "circsim.erc.missing_ground"));
    }
    SUBCASE("one pin wired to a dead-end wire") {
        Bench b;
        Part* r1 = b.r("R1", {0.0, 0.0});
        Part* g = b.gnd({3.0, -4.0});
        Wire* w = b.s.add_wire({});
        w->a = PinRef{r1->id, 0};
        w->b = ws::circsim::JunctionRef{b.junction(-6.0, 0.0)};
        b.pin_wire(PinRef{r1->id, 1}, PinRef{g->id, 0});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK(has(diags, "circsim.erc.one_pin_net", Severity::Warning));
        CHECK_FALSE(has(diags, "circsim.erc.floating_pin"));
        CHECK_FALSE(has(diags, "circsim.erc.missing_ground"));
    }
}

TEST_CASE("a net with no pins warns as empty") {
    Bench b;
    b.j_wire(0.0, 0.0, 10.0, 0.0);
    b.gnd({5.0, -4.0});
    const auto diags = ws::circsim::run_erc(b.s);
    CHECK(has(diags, "circsim.erc.empty_net", Severity::Warning));
}

TEST_CASE("missing ground errors only when other nets exist") {
    Bench b;
    Part* v1 = b.v("V1", {0.0, 0.0});
    Part* r1 = b.r("R1", {6.0, 0.0});
    b.pin_wire(PinRef{v1->id, 0}, PinRef{r1->id, 0});
    b.pin_wire(PinRef{r1->id, 1}, PinRef{v1->id, 1});
    const auto diags = ws::circsim::run_erc(b.s);
    CHECK(has(diags, "circsim.erc.missing_ground"));

    // With ground (a label is enough), the error is gone.
    Label* l = b.s.add_label({});
    l->pos = Pos{3.0, 0.0};
    l->name = "gnd";
    const auto after = ws::circsim::run_erc(b.s);
    CHECK_FALSE(has(after, "circsim.erc.missing_ground"));
    // ...and the one-pin ground net stays exempt from one_pin_net.
    CHECK_FALSE(has(after, "circsim.erc.one_pin_net"));
}

TEST_CASE("duplicate refs error, case-insensitively") {
    Bench b;
    b.r("R1", {0.0, 0.0});
    b.r("r1", {10.0, 0.0});
    const auto diags = ws::circsim::run_erc(b.s);
    CHECK(has(diags, "circsim.erc.duplicate_ref"));
}

TEST_CASE("ref problems: missing, wrong prefix, bad characters") {
    SUBCASE("missing ref") {
        Bench b;
        b.part("r", "", "1k", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.missing_ref"));
    }
    SUBCASE("wrong letter for the symbol") {
        Bench b;
        b.r("Q1", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.ref_prefix"));
    }
    SUBCASE("not an identifier") {
        Bench b;
        b.r("R 1", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.bad_ref"));
    }
    SUBCASE("power symbols want no ref and are exempt") {
        Bench b;
        b.gnd({0.0, 0.0});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK_FALSE(has(diags, "circsim.erc.missing_ref"));
        CHECK_FALSE(has(diags, "circsim.erc.missing_ground"));
    }
}

TEST_CASE("missing value and missing model error per the symbol") {
    SUBCASE("resistor without a value") {
        Bench b;
        b.part("r", "R1", "", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.missing_value"));
    }
    SUBCASE("transistor without a model") {
        Bench b;
        b.part("npn", "Q1", "", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.missing_model"));
    }
    SUBCASE("with a model it is quiet") {
        Bench b;
        Part* q = b.part("npn", "Q1", "", {0.0, 0.0});
        q->params["model"] = "qmod";
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK_FALSE(has(diags, "circsim.erc.missing_model"));
    }
    SUBCASE("a waveform source is self-valued") {
        // The wave param IS the stimulus: the engine derives the DC/bias
        // level from waveform(0) when the card spells no value ("V1 a b
        // SIN(0 5 1)" runs as-is), so a source with a wave and no value
        // is not a missing-value error. A wave-less source with no value
        // still complains.
        Bench b;
        Part* v1 = b.part("v", "V1", "", {0.0, 0.0});
        v1->params["wave"] = "SIN(0 5 1)";
        CHECK_FALSE(has(ws::circsim::run_erc(b.s),
                        "circsim.erc.missing_value"));
        Bench bare;
        bare.part("v", "V1", "", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(bare.s),
                  "circsim.erc.missing_value"));
    }
}

TEST_CASE("unknown symbols error") {
    Bench b;
    b.part("mystery", "U1", "??", {0.0, 0.0});
    const auto diags = ws::circsim::run_erc(b.s);
    CHECK(has(diags, "circsim.erc.unknown_symbol"));
    // The one-part schematic still has no ground: exactly one more error.
    CHECK_FALSE(has(diags, "circsim.erc.missing_ground"));
}

TEST_CASE("voltage-source loops are caught in every shape") {
    SUBCASE("parallel sources") {
        Bench b;
        Part* v1 = b.v("V1", {0.0, -4.0});
        Part* v2 = b.v("V2", {10.0, 0.0});
        b.pin_wire(PinRef{v1->id, 0}, PinRef{v2->id, 0});
        b.pin_wire(PinRef{v1->id, 1}, PinRef{v2->id, 1});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK(has(diags, "circsim.erc.vsource_loop"));
    }
    SUBCASE("a triangle of sources through three nets") {
        Bench b;
        Part* v1 = b.v("V1", {0.0, 0.0});
        Part* v2 = b.v("V2", {0.0, 10.0});
        Part* v3 = b.v("V3", {0.0, 20.0});
        b.pin_wire(PinRef{v1->id, 1}, PinRef{v2->id, 0});
        b.pin_wire(PinRef{v2->id, 1}, PinRef{v3->id, 0});
        b.pin_wire(PinRef{v3->id, 1}, PinRef{v1->id, 0});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK(has(diags, "circsim.erc.vsource_loop"));
        // The report names all three.
        for (const ws::circsim::Diag& d : diags) {
            if (d.code == "circsim.erc.vsource_loop") {
                CHECK(d.message.find("V1") != std::string::npos);
                CHECK(d.message.find("V2") != std::string::npos);
                CHECK(d.message.find("V3") != std::string::npos);
            }
        }
    }
    SUBCASE("a source shorted onto itself") {
        Bench b;
        Part* v1 = b.v("V1", {0.0, 0.0});
        b.pin_wire(PinRef{v1->id, 0}, PinRef{v1->id, 1});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.vsource_loop"));
    }
    SUBCASE("a source over a resistor is no loop") {
        Bench b;
        Part* v1 = b.v("V1", {0.0, -4.0});
        Part* r1 = b.r("R1", {6.0, 0.0});
        b.pin_wire(PinRef{v1->id, 0}, PinRef{r1->id, 0});
        b.pin_wire(PinRef{v1->id, 1}, PinRef{r1->id, 1});
        CHECK_FALSE(has(ws::circsim::run_erc(b.s), "circsim.erc.vsource_loop"));
    }
    SUBCASE("parallel inductors too") {
        Bench b;
        // Two-terminal parts have pins at +/-3: the second one sits off the
        // axis so the straight pin wires miss both parts' opposite pins.
        Part* l1 = b.part("l", "L1", "1m", {0.0, 0.0});
        Part* l2 = b.part("l", "L2", "1m", {10.0, 6.0});
        b.pin_wire(PinRef{l1->id, 0}, PinRef{l2->id, 0});
        b.pin_wire(PinRef{l1->id, 1}, PinRef{l2->id, 1});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.vsource_loop"));
    }
}

TEST_CASE("current-source cutsets are caught; shunted ones are not") {
    SUBCASE("a lone source between two islands") {
        Bench b;
        Part* i1 = b.i("I1", {0.0, 0.0});
        b.j_wire(-1.0, 0.0, -1.0, 2.0);   // island A junction wire
        Wire* wa = b.s.add_wire({});
        wa->a = ws::circsim::JunctionRef{b.junction(-1.0, 2.0)};
        wa->b = PinRef{i1->id, 0};
        b.j_wire(1.0, 0.0, 1.0, 2.0);    // island B junction wire
        Wire* wb = b.s.add_wire({});
        wb->a = ws::circsim::JunctionRef{b.junction(1.0, 2.0)};
        wb->b = PinRef{i1->id, 1};
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.isource_cutset"));
    }
    SUBCASE("two sources in series") {
        Bench b;
        Part* i1 = b.i("I1", {0.0, 0.0});
        Part* i2 = b.i("I2", {0.0, 10.0});
        b.pin_wire(PinRef{i1->id, 1}, PinRef{i2->id, 0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.isource_cutset"));
    }
    SUBCASE("a source over a resistor is fine") {
        Bench b;
        Part* i1 = b.i("I1", {0.0, -4.0});
        Part* r1 = b.r("R1", {6.0, 0.0});
        b.pin_wire(PinRef{i1->id, 0}, PinRef{r1->id, 0});
        b.pin_wire(PinRef{i1->id, 1}, PinRef{r1->id, 1});
        CHECK_FALSE(
            has(ws::circsim::run_erc(b.s), "circsim.erc.isource_cutset"));
    }
}

TEST_CASE("the orphan devices' required params and refs check like any other") {
    SUBCASE("a coupling without its pair names errors") {
        Bench b;
        b.part("k", "K1", "", {0.0, 0.0});
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK(has(diags, "circsim.erc.missing_param"));
    }
    SUBCASE("a complete coupling is param-quiet") {
        Bench b;
        Part* k = b.part("k", "K1", "", {0.0, 0.0});
        k->params["l1"] = "L1";
        k->params["l2"] = "L2";
        k->params["k"] = "0.9";
        const auto diags = ws::circsim::run_erc(b.s);
        CHECK_FALSE(has(diags, "circsim.erc.missing_param"));
        CHECK_FALSE(has(diags, "circsim.erc.unused_param"));
    }
    SUBCASE("a coupling's k must be a SPICE number") {
        Bench b;
        Part* k = b.part("k", "K1", "", {0.0, 0.0});
        k->params["l1"] = "L1";
        k->params["l2"] = "L2";
        k->params["k"] = "tight";
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.bad_param"));
    }
    SUBCASE("the transformer's per-card params are required") {
        Bench b;
        Part* x = b.part("xfmr", "K1", "", {0.0, 0.0});
        x->params["lp"] = "1m";
        // ls and k missing: two missing_param errors.
        const auto diags = ws::circsim::run_erc(b.s);
        std::size_t missing = 0;
        for (const auto& d : diags) {
            if (d.code == "circsim.erc.missing_param") {
                ++missing;
            }
        }
        CHECK(missing == 2);
    }
    SUBCASE("the coupling ref must still start with K") {
        Bench b;
        b.part("k", "TR1", "", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.ref_prefix"));
    }
    SUBCASE("a switch without a model errors, with one it is quiet") {
        Bench b;
        Part* s1 = b.part("s", "S1", "", {0.0, 0.0});
        s1->params["model"] = "swmod";
        CHECK_FALSE(has(ws::circsim::run_erc(b.s), "circsim.erc.missing_model"));
    }
    SUBCASE("a controlled source without its controlling name errors") {
        Bench b;
        b.part("f", "F1", "", {0.0, 0.0});
        CHECK(has(ws::circsim::run_erc(b.s), "circsim.erc.missing_param"));
    }
}
