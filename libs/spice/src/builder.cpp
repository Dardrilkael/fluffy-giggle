// CircuitBuilder (DESIGN.md §2.1 rev): assembles device CARDS - the same
// token stream the netlist parser emits - and runs them through the same
// elaborator. The builder is not a parallel implementation of anything:
// arity, validation, models, coupling resolution and the graph checks are
// the parser's code paths, exercised verbatim. That is what makes the
// builder-vs-netlist equivalence a test, not a hope.

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cards.hpp"

#include <ws/spice/builder.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/parse.hpp>

namespace ws::spice {

namespace {

// Shortest exactly-round-tripping number token (same spelling the netlist
// writer uses, so written decks and built cards agree bit-for-bit).
std::string num(double v) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec != std::errc{}) {
        return "0";
    }
    return std::string(buf, res.ptr);
}

Diagnostic builder_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}


// One builder Value -> its card token (a number, or the expression
// spelling as-is for the elaborator's scope to resolve).
std::string value_token(const Value& v) {
    if (const double* d = std::get_if<double>(&v)) {
        return num(*d);
    }
    return std::get<ExprVal>(v).text;
}

DeviceCard make_card(const std::string& name, const std::vector<std::string>& rest) {
    DeviceCard card;
    card.name.text = name;
    card.name.loc = SourceLoc{};  // invalid: no file behind the builder
    card.loc = SourceLoc{};
    card.rest.reserve(rest.size());
    for (const std::string& tok : rest) {
        Token t;
        t.text = tok;
        card.rest.push_back(std::move(t));
    }
    return card;
}

std::vector<std::string> waveform_tokens(const SourceValue& value) {
    std::vector<std::string> out;
    if (const double* dc = std::get_if<double>(&value)) {
        out.push_back("DC");
        out.push_back(num(*dc));
        return out;
    }
    if (const ExprVal* e = std::get_if<ExprVal>(&value)) {
        // A scoped spelling: the elaborator resolves it; DC-anonymous like
        // the card form (an explicit 'DC' token precedes it).
        out.push_back("DC");
        out.push_back(e->text);
        return out;
    }
    auto push_all = [&](const char* name, const std::vector<double>& ps) {
        std::string card = std::string(name) + "(";
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i != 0) {
                card += " ";
            }
            card += num(ps[i]);
        }
        card += ")";
        out.push_back(std::move(card));
    };
    if (const Pulse* p = std::get_if<Pulse>(&value)) {
        push_all("PULSE", {p->v1, p->v2, p->td, p->tr, p->tf, p->pw, p->per});
    } else if (const Sin* s = std::get_if<Sin>(&value)) {
        push_all("SIN", {s->vo, s->va, s->freq, s->td, s->theta, s->phase});
    } else if (const ExpSource* e = std::get_if<ExpSource>(&value)) {
        push_all("EXP", {e->v1, e->v2, e->td1, e->tau1, e->td2, e->tau2});
    } else if (const Pwl* pwl = std::get_if<Pwl>(&value)) {
        std::string card = "PWL(";
        for (std::size_t i = 0; i < pwl->points.size(); ++i) {
            if (i != 0) {
                card += " ";
            }
            card += num(pwl->points[i].first) + " " + num(pwl->points[i].second);
        }
        card += ")";
        out.push_back(std::move(card));
    }
    return out;
}

void push_ac(std::vector<std::string>& rest, const std::optional<AcSpec>& ac) {
    if (!ac) {
        return;
    }
    rest.push_back("AC");
    rest.push_back(num(ac->magnitude));
    if (ac->phase_deg != 0.0) {
        rest.push_back(num(ac->phase_deg));
    }
}

}  // namespace

NodeHandle CircuitBuilder::node(std::string name) {
    return NodeHandle{std::move(name)};
}

void CircuitBuilder::param(std::string name, std::string expression) {
    params_.emplace_back(std::move(name), std::move(expression));
}

DeviceHandle CircuitBuilder::resistor(std::string name, NodeHandle a, NodeHandle b,
                                      Value ohms) {
    cards_.push_back({name, {a.name, b.name, value_token(ohms)}});
    return DeviceHandle{name, ParamHandle{name, "value"}};
}

DeviceHandle CircuitBuilder::capacitor(std::string name, NodeHandle a, NodeHandle b,
                                       Value farads, std::optional<double> ic) {
    std::vector<std::string> rest{a.name, b.name, value_token(farads)};
    if (ic) {
        rest.push_back("ic=" + num(*ic));
    }
    cards_.push_back({std::move(name), std::move(rest)});
    return DeviceHandle{name, ParamHandle{name, "value"}};
}

DeviceHandle CircuitBuilder::inductor(std::string name, NodeHandle a, NodeHandle b,
                                      Value henries, std::optional<double> ic,
                                      std::optional<double> rser) {
    std::vector<std::string> rest{a.name, b.name, value_token(henries)};
    if (rser) {
        rest.push_back("Rser=" + num(*rser));
    }
    if (ic) {
        rest.push_back("ic=" + num(*ic));
    }
    cards_.push_back({std::move(name), std::move(rest)});
    return DeviceHandle{name, ParamHandle{name, "value"}};
}

void CircuitBuilder::coupling(std::string name, std::string inductor_a,
                              std::string inductor_b, double k) {
    cards_.push_back({std::move(name), {std::move(inductor_a), std::move(inductor_b),
                                        num(k)}});
}

DeviceHandle CircuitBuilder::vsource(std::string name, NodeHandle p, NodeHandle n,
                                     SourceValue value, std::optional<AcSpec> ac) {
    std::vector<std::string> rest{p.name, n.name};
    for (std::string& tok : waveform_tokens(value)) {
        rest.push_back(std::move(tok));
    }
    push_ac(rest, ac);
    cards_.push_back({std::move(name), std::move(rest)});
    return DeviceHandle{name, ParamHandle{name, "dc"}};
}

DeviceHandle CircuitBuilder::isource(std::string name, NodeHandle p, NodeHandle n,
                                     SourceValue value, std::optional<AcSpec> ac) {
    std::vector<std::string> rest{p.name, n.name};
    for (std::string& tok : waveform_tokens(value)) {
        rest.push_back(std::move(tok));
    }
    push_ac(rest, ac);
    cards_.push_back({std::move(name), std::move(rest)});
    return DeviceHandle{name, ParamHandle{name, "dc"}};
}

DeviceHandle CircuitBuilder::vcvs(std::string name, NodeHandle p, NodeHandle n,
                                  NodeHandle cp, NodeHandle cn, Value gain) {
    cards_.push_back({std::move(name), {p.name, n.name, cp.name, cn.name, value_token(gain)}});
    return DeviceHandle{name, ParamHandle{name, "gain"}};
}

DeviceHandle CircuitBuilder::vccs(std::string name, NodeHandle p, NodeHandle n,
                                  NodeHandle cp, NodeHandle cn, Value gm) {
    cards_.push_back({std::move(name), {p.name, n.name, cp.name, cn.name, value_token(gm)}});
    return DeviceHandle{name, ParamHandle{name, "gm"}};
}

DeviceHandle CircuitBuilder::cccs(std::string name, NodeHandle p, NodeHandle n,
                                  std::string controlling_vsource, Value gain) {
    cards_.push_back(
        {std::move(name), {p.name, n.name, std::move(controlling_vsource), value_token(gain)}});
    return DeviceHandle{name, ParamHandle{name, "gain"}};
}

DeviceHandle CircuitBuilder::ccvs(std::string name, NodeHandle p, NodeHandle n,
                                  std::string controlling_vsource, Value r) {
    cards_.push_back(
        {std::move(name), {p.name, n.name, std::move(controlling_vsource), value_token(r)}});
    return DeviceHandle{name, ParamHandle{name, "r"}};
}

void CircuitBuilder::vswitch(std::string name, NodeHandle p, NodeHandle n, NodeHandle cp,
                             NodeHandle cn, std::string model, bool initial_on) {
    std::vector<std::string> rest{p.name, n.name, cp.name, cn.name, std::move(model)};
    if (initial_on) {
        rest.push_back("ic=on");
    }
    cards_.push_back({std::move(name), std::move(rest)});
}

void CircuitBuilder::iswitch(std::string name, NodeHandle p, NodeHandle n,
                             std::string controlling_vsource, std::string model,
                             bool initial_on) {
    std::vector<std::string> rest{p.name, n.name, std::move(controlling_vsource),
                                  std::move(model)};
    if (initial_on) {
        rest.push_back("ic=on");
    }
    cards_.push_back({std::move(name), std::move(rest)});
}

void CircuitBuilder::switch_model(std::string name, const SwitchModel& m) {
    models_.emplace_back(std::move(name), m);
}

ws::core::Result<Circuit> CircuitBuilder::finish(double temp,
                                                 std::vector<Warning>* warnings) {
    std::vector<Warning> scratch;
    std::vector<Warning>& w = warnings ? *warnings : scratch;

    DeckAst ast;
    ast.title = "builder circuit";
    for (const auto& [name, expr] : params_) {
        const ws::core::Result<ExprProgram> prog = parse_expression(expr);
        if (!prog) {
            Diagnostic d = builder_diag(ErrorKind::Parse, "spice.parse.expr",
                                         "param '" + name + "': " + prog.error().message);
            return spice_fail(std::move(d));
        }
        ParamDef def;
        def.name.text = name;
        def.expr = *prog;
        ast.params.push_back(std::move(def));
    }
    for (const Card& c : cards_) {
        ast.devices.push_back(make_card(c.name, c.rest));
    }
    for (const auto& [name, m] : models_) {
        ModelDef def;
        def.name.text = name;
        def.type = "sw";
        auto kv = [&](const char* key, double v) {
            Token kt;
            kt.text = key;
            Token vt;
            vt.text = num(v);
            def.params.push_back(std::move(kt));
            def.params.push_back(std::move(vt));
        };
        kv("Ron", m.ron);
        kv("Roff", m.roff);
        if (m.vt) {
            kv("Vt", *m.vt);
        }
        if (m.vh) {
            kv("Vh", *m.vh);
        }
        if (m.it) {
            kv("It", *m.it);
        }
        if (m.ih) {
            kv("Ih", *m.ih);
        }
        ast.models.push_back(std::move(def));
    }
    return elaborate_deck(ast, temp, w);
}

}  // namespace ws::spice
