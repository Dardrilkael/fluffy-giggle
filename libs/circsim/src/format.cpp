#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ws/circsim/format.hpp>

namespace ws::circsim {

namespace json = ws::settings::json;

namespace {

// --- small json helpers ------------------------------------------------------

std::optional<double> as_number(const json::Value* v) {
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->is_integer()) {
        const auto n = v->get_integer();
        if (n) {
            return static_cast<double>(*n);
        }
        return std::nullopt;
    }
    if (v->is_real()) {
        const auto n = v->get_real();
        if (n) {
            return *n;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> as_integer(const json::Value* v) {
    if (v != nullptr && v->is_integer()) {
        const auto n = v->get_integer();
        if (n) {
            return *n;
        }
    }
    return std::nullopt;
}

std::optional<std::string> as_text(const json::Value* v) {
    if (v != nullptr && v->is_text()) {
        const auto t = v->get_text();
        if (t) {
            return *t;
        }
    }
    return std::nullopt;
}

std::optional<bool> as_bool(const json::Value* v) {
    if (v != nullptr && v->is_bool()) {
        const auto b = v->get_bool();
        if (b) {
            return *b;
        }
    }
    return std::nullopt;
}

std::optional<Pos> as_pos(const json::Value* v) {
    if (v == nullptr || !v->is_array()) {
        return std::nullopt;
    }
    const json::Array* a = v->as_array();
    if (a == nullptr || a->size() != 2) {
        return std::nullopt;
    }
    const auto x = as_number(&(*a)[0]);
    const auto y = as_number(&(*a)[1]);
    if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
    }
    return Pos{*x, *y};
}

json::Value pos_value(Pos p) {
    json::Array a;
    a.push_back(json::Value::real(p.x));
    a.push_back(json::Value::real(p.y));
    return json::Value::array(std::move(a));
}

Diag load_diag(std::string code, std::string message, std::string where) {
    Diag d;
    d.severity = Severity::Warning;
    d.code = std::move(code);
    d.message = std::move(message);
    d.where = std::move(where);
    return d;
}

// --- migration chain -----------------------------------------------------------
// kMigrations[i] carries version i -> i+1. v0 is "no version field at all".
// Steps may edit the raw root; unknown fields ride through untouched.

void migrate_0_to_1(json::Object& root, std::vector<Diag>& diags) {
    if (root.find("directives") == nullptr) {
        root.set("directives", json::Value::text(""));
    }
    if (root.find("analysis") == nullptr) {
        root.set("analysis", json::Value::object());
    }
    diags.push_back(load_diag(
        "circsim.format.migrated",
        "file carried no version; migrated to version 1 (defaults filled)",
        ""));
}

using Migrator = void (*)(json::Object&, std::vector<Diag>&);
constexpr Migrator kMigrations[] = {
    migrate_0_to_1,
};
constexpr int kOldestVersion = 0;

// --- entity readers ---------------------------------------------------------------

// Reads the recognized fields of one object, moving everything else into
// `unknown`. Wrong-typed known fields repair to defaults with a diagnostic.
struct PartReader {
    Part p;
    bool ok = false;
};

PartReader read_part(const json::Object& o, std::vector<Diag>& diags) {
    PartReader r;
    const auto id = as_integer(o.find("id"));
    if (!id.has_value() || *id <= 0 || *id > 0xFFFFFFFFLL) {
        diags.push_back(load_diag(
            "circsim.format.bad_id",
            "part without a usable id is dropped", ""));
        return r;
    }
    r.p.id = static_cast<std::uint32_t>(*id);
    r.ok = true;
    if (const auto sym = as_text(o.find("symbol"))) {
        r.p.symbol = *sym;
    } else {
        diags.push_back(load_diag(
            "circsim.format.bad_field", "part " + std::to_string(r.p.id) +
            ": 'symbol' is missing or not text; defaulting to empty",
            "part " + std::to_string(r.p.id)));
    }
    if (const auto ref = as_text(o.find("ref"))) {
        r.p.ref = *ref;
    }
    if (const json::Value* value = o.find("value"); value != nullptr) {
        if (const auto text = as_text(value)) {
            r.p.value = *text;
        } else {
            diags.push_back(load_diag(
                "circsim.format.bad_field",
                "part " + std::to_string(r.p.id) +
                    ": 'value' is not text; cleared",
                "part " + std::to_string(r.p.id)));
        }
    }
    if (const auto pos = as_pos(o.find("pos"))) {
        r.p.pos = *pos;
    } else {
        diags.push_back(load_diag(
            "circsim.format.bad_field",
            "part " + std::to_string(r.p.id) +
                ": 'pos' is missing or not [x, y]; defaulting to [0, 0]",
            "part " + std::to_string(r.p.id)));
    }
    if (const auto rot = as_integer(o.find("rot"))) {
        r.p.rot = static_cast<int>(*rot);
    }
    if (const auto mirror = as_bool(o.find("mirror"))) {
        r.p.mirror = *mirror;
    }
    if (const json::Value* params = o.find("params");
        params != nullptr && params->is_object()) {
        for (const auto& [key, val] : *params->as_object()) {
            if (const auto text = as_text(&val)) {
                r.p.params[key] = *text;
            } else {
                diags.push_back(load_diag(
                    "circsim.format.bad_field",
                    "part " + std::to_string(r.p.id) + ": param '" + key +
                        "' is not text; dropped",
                    "part " + std::to_string(r.p.id)));
            }
        }
    }
    for (const auto& [key, val] : o) {
        if (key != "id" && key != "symbol" && key != "ref" && key != "value" &&
            key != "pos" && key != "rot" && key != "mirror" &&
            key != "params") {
            r.p.unknown.set(key, val);
        }
    }
    return r;
}

bool read_wire(const json::Object& o, Wire& w, std::vector<Diag>& diags) {
    const auto id = as_integer(o.find("id"));
    if (!id.has_value() || *id <= 0 || *id > 0xFFFFFFFFLL) {
        diags.push_back(load_diag("circsim.format.bad_id",
                                  "wire without a usable id is dropped", ""));
        return false;
    }
    w.id = static_cast<std::uint32_t>(*id);

    const auto read_endpoint = [&](const char* key,
                                   WireEndpoint& out) -> bool {
        const json::Value* v = o.find(key);
        if (v == nullptr || !v->is_object()) {
            return false;
        }
        const json::Object& eo = *v->as_object();
        if (const json::Value* pin = eo.find("pin");
            pin != nullptr && pin->is_object()) {
            const json::Object& po = *pin->as_object();
            const auto part = as_integer(po.find("part"));
            const auto index = as_integer(po.find("pin"));
            if (part.has_value() && index.has_value() && *part > 0 &&
                *index >= 0) {
                out = PinRef{static_cast<std::uint32_t>(*part),
                             static_cast<std::uint32_t>(*index)};
                return true;
            }
            return false;
        }
        if (const json::Value* jun = eo.find("junction");
            jun != nullptr && jun->is_object()) {
            const auto jid = as_integer(jun->as_object()->find("id"));
            if (jid.has_value() && *jid > 0) {
                out = JunctionRef{static_cast<std::uint32_t>(*jid)};
                return true;
            }
            return false;
        }
        return false;
    };

    if (!read_endpoint("a", w.a) || !read_endpoint("b", w.b)) {
        diags.push_back(load_diag(
            "circsim.format.bad_field",
            "wire " + std::to_string(w.id) +
                ": endpoint is missing or malformed; wire dropped",
            "wire " + std::to_string(w.id)));
        return false;
    }
    if (const json::Value* wps = o.find("waypoints");
        wps != nullptr && wps->is_array()) {
        for (const json::Value& v : *wps->as_array()) {
            if (const auto p = as_pos(&v)) {
                w.waypoints.push_back(*p);
            } else {
                diags.push_back(load_diag(
                    "circsim.format.bad_field",
                    "wire " + std::to_string(w.id) +
                        ": a waypoint is not [x, y]; dropped",
                    "wire " + std::to_string(w.id)));
            }
        }
    }
    if (const auto ortho = as_bool(o.find("ortho"))) {
        w.ortho = *ortho;
    } else {
        w.ortho = true;
    }
    for (const auto& [key, val] : o) {
        if (key != "id" && key != "a" && key != "b" && key != "waypoints" &&
            key != "ortho") {
            w.unknown.set(key, val);
        }
    }
    return true;
}

// --- analysis setup ---------------------------------------------------------------

void write_analysis(json::Object& out, const AnalysisSetup& setup) {
    json::Array entries;
    for (const AnalysisSpec& spec : setup.entries) {
        json::Object e;
        if (const OpSpec* op = std::get_if<OpSpec>(&spec)) {
            (void)op;
            e.set("op", json::Value::object());
        } else if (const DcSpec* dc = std::get_if<DcSpec>(&spec)) {
            json::Object d;
            d.set("source", json::Value::text(dc->source));
            d.set("start", json::Value::real(dc->start));
            d.set("stop", json::Value::real(dc->stop));
            d.set("step", json::Value::real(dc->step));
            e.set("dc", json::Value::object(std::move(d)));
        } else if (const TranSpec* tran = std::get_if<TranSpec>(&spec)) {
            json::Object t;
            t.set("tstep", json::Value::real(tran->tstep));
            t.set("tstop", json::Value::real(tran->tstop));
            if (tran->tstart.has_value()) {
                t.set("tstart", json::Value::real(*tran->tstart));
            }
            if (tran->tmax.has_value()) {
                t.set("tmax", json::Value::real(*tran->tmax));
            }
            if (tran->uic) {
                t.set("uic", json::Value::boolean(true));
            }
            e.set("tran", json::Value::object(std::move(t)));
        } else if (const AcSpec* ac = std::get_if<AcSpec>(&spec)) {
            json::Object a;
            const char* sweep = "dec";
            if (ac->sweep == AcSpec::Sweep::Oct) {
                sweep = "oct";
            } else if (ac->sweep == AcSpec::Sweep::Lin) {
                sweep = "lin";
            }
            a.set("sweep", json::Value::text(sweep));
            a.set("n", json::Value::integer(ac->n));
            a.set("fstart", json::Value::real(ac->fstart));
            a.set("fstop", json::Value::real(ac->fstop));
            e.set("ac", json::Value::object(std::move(a)));
        } else if (const NoiseSpec* ns = std::get_if<NoiseSpec>(&spec)) {
            json::Object o;
            const char* sweep = "dec";
            if (ns->sweep == NoiseSpec::Sweep::Oct) {
                sweep = "oct";
            } else if (ns->sweep == NoiseSpec::Sweep::Lin) {
                sweep = "lin";
            }
            o.set("output", json::Value::text(ns->output));
            if (!ns->ref.empty()) {
                o.set("ref", json::Value::text(ns->ref));
            }
            o.set("input", json::Value::text(ns->input));
            o.set("sweep", json::Value::text(sweep));
            o.set("n", json::Value::integer(ns->n));
            o.set("fstart", json::Value::real(ns->fstart));
            o.set("fstop", json::Value::real(ns->fstop));
            if (ns->pts_per_summary > 0) {
                o.set("pts", json::Value::integer(ns->pts_per_summary));
            }
            e.set("noise", json::Value::object(std::move(o)));
        }
        entries.push_back(json::Value::object(std::move(e)));
    }
    json::Object root;
    root.set("entries", json::Value::array(std::move(entries)));
    out.set("analysis", json::Value::object(std::move(root)));
}

AnalysisSetup read_analysis(const json::Value* v, std::vector<Diag>& diags) {
    AnalysisSetup setup;
    if (v == nullptr) {
        return setup;
    }
    if (!v->is_object()) {
        diags.push_back(load_diag("circsim.format.bad_field",
                                  "'analysis' is not an object; ignored", ""));
        return setup;
    }
    const json::Value* entries = v->as_object()->find("entries");
    if (entries == nullptr || !entries->is_array()) {
        return setup;
    }
    for (const json::Value& e : *entries->as_array()) {
        if (!e.is_object() || e.as_object()->size() != 1) {
            diags.push_back(load_diag(
                "circsim.format.bad_field",
                "an analysis entry is not a one-key object; skipped", ""));
            continue;
        }
        const auto& [kind, body] = *e.as_object()->begin();
        const json::Object* o =
            body.is_object() ? body.as_object() : nullptr;
        if (kind == "op") {
            setup.entries.push_back(OpSpec{});
        } else if (kind == "dc" && o != nullptr) {
            DcSpec dc;
            if (const auto src = as_text(o->find("source"))) {
                dc.source = *src;
            }
            if (const auto n = as_number(o->find("start"))) {
                dc.start = *n;
            }
            if (const auto n = as_number(o->find("stop"))) {
                dc.stop = *n;
            }
            if (const auto n = as_number(o->find("step"))) {
                dc.step = *n;
            }
            setup.entries.push_back(dc);
        } else if (kind == "tran" && o != nullptr) {
            TranSpec t;
            if (const auto n = as_number(o->find("tstep"))) {
                t.tstep = *n;
            }
            if (const auto n = as_number(o->find("tstop"))) {
                t.tstop = *n;
            }
            if (const auto n = as_number(o->find("tstart"))) {
                t.tstart = *n;
            }
            if (const auto n = as_number(o->find("tmax"))) {
                t.tmax = *n;
            }
            if (const auto b = as_bool(o->find("uic"))) {
                t.uic = *b;
            }
            setup.entries.push_back(t);
        } else if (kind == "ac" && o != nullptr) {
            AcSpec ac;
            if (const auto sw = as_text(o->find("sweep"))) {
                if (*sw == "oct") {
                    ac.sweep = AcSpec::Sweep::Oct;
                } else if (*sw == "lin") {
                    ac.sweep = AcSpec::Sweep::Lin;
                }
            }
            if (const auto n = as_integer(o->find("n"))) {
                ac.n = static_cast<int>(*n);
            }
            if (const auto n = as_number(o->find("fstart"))) {
                ac.fstart = *n;
            }
            if (const auto n = as_number(o->find("fstop"))) {
                ac.fstop = *n;
            }
            setup.entries.push_back(ac);
        } else if (kind == "noise" && o != nullptr) {
            NoiseSpec ns;
            if (const auto t = as_text(o->find("output"))) {
                ns.output = *t;
            }
            if (const auto t = as_text(o->find("ref"))) {
                ns.ref = *t;
            }
            if (const auto t = as_text(o->find("input"))) {
                ns.input = *t;
            }
            if (const auto sw = as_text(o->find("sweep"))) {
                if (*sw == "oct") {
                    ns.sweep = NoiseSpec::Sweep::Oct;
                } else if (*sw == "lin") {
                    ns.sweep = NoiseSpec::Sweep::Lin;
                }
            }
            if (const auto n = as_integer(o->find("n"))) {
                ns.n = static_cast<int>(*n);
            }
            if (const auto n = as_number(o->find("fstart"))) {
                ns.fstart = *n;
            }
            if (const auto n = as_number(o->find("fstop"))) {
                ns.fstop = *n;
            }
            if (const auto n = as_integer(o->find("pts"))) {
                ns.pts_per_summary = static_cast<int>(*n);
            }
            setup.entries.push_back(ns);
        } else {
            diags.push_back(load_diag(
                "circsim.format.bad_field",
                "unknown analysis kind '" + kind + "'; entry skipped", ""));
        }
    }
    return setup;
}

// --- the entity-array readers (tolerant: duplicate ids, dangling refs) --------------

template <typename T, typename Read>
void read_array(const json::Value* v, const char* what,
                std::deque<T>& out, std::vector<Diag>& diags,
                std::vector<std::uint32_t>& seen_ids, Read&& read_one) {
    if (v == nullptr) {
        return;
    }
    if (!v->is_array()) {
        diags.push_back(load_diag(
            "circsim.format.bad_field",
            std::string("'") + what + "' is not an array; treated as empty",
            ""));
        return;
    }
    for (const json::Value& e : *v->as_array()) {
        if (!e.is_object()) {
            diags.push_back(load_diag(
                "circsim.format.bad_field",
                std::string("a ") + what + " entry is not an object; skipped",
                ""));
            continue;
        }
        T obj;
        if (!read_one(*e.as_object(), obj, diags)) {
            continue;
        }
        if (std::find(seen_ids.begin(), seen_ids.end(), obj.id) !=
            seen_ids.end()) {
            diags.push_back(load_diag(
                "circsim.format.duplicate_id",
                std::string(what) + " id " + std::to_string(obj.id) +
                    " appears twice; the first one wins",
                std::string(what) + " " + std::to_string(obj.id)));
            continue;
        }
        seen_ids.push_back(obj.id);
        out.push_back(std::move(obj));
    }
}

}  // namespace

ws::core::Result<Schematic> load_csim(std::string_view text,
                                      LoadReport& report) {
    const auto parsed = json::parse(text);
    if (!parsed) {
        return ws::core::unexpected<ws::core::Error>(
            ws::core::make_error(1, "circsim",
                                 "not a .csim file: " + parsed.error().message));
    }
    if (!parsed->is_object()) {
        return ws::core::unexpected<ws::core::Error>(
            ws::core::make_error(1, "circsim",
                                 "not a .csim file: the top level is not an object"));
    }
    json::Object root = *parsed->as_object();

    const auto format = as_text(root.find("format"));
    if (!format.has_value() || *format != kCsimFormat) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            1, "circsim",
            "not a .csim file: \"format\" is not \"" +
                std::string(kCsimFormat) + "\""));
    }

    int version = kOldestVersion;
    if (const auto v = as_integer(root.find("version"))) {
        version = static_cast<int>(*v);
    }
    report.loaded_version = version;
    if (version > kCsimVersion) {
        report.future = true;
        report.diags.push_back(load_diag(
            "circsim.format.future_version",
            "file says version " + std::to_string(version) +
                ", this build knows " + std::to_string(kCsimVersion) +
                "; reading what it understands",
            ""));
    } else if (version < kCsimVersion) {
        for (int v = version; v < kCsimVersion; ++v) {
            kMigrations[v - kOldestVersion](root, report.diags);
        }
        report.migrated = true;
    }

    Schematic s;
    if (const json::Value* grid = root.find("grid"); grid != nullptr) {
        if (const auto n = as_number(grid)) {
            s.grid = *n;
        } else {
            report.diags.push_back(load_diag(
                "circsim.format.bad_field",
                "'grid' is not a number; defaulting to 1", ""));
        }
    }
    if (const auto directives = as_text(root.find("directives"))) {
        s.directives = *directives;
    }
    // §7.7 (M5a): the run temperature. A non-number repairs to unset
    // (SPICE's default) with a diagnostic, the loader's own contract.
    if (const json::Value* temp = root.find("temp"); temp != nullptr) {
        if (const auto n = as_number(temp)) {
            s.temp = *n;
        } else {
            report.diags.push_back(load_diag(
                "circsim.format.bad_field",
                "'temp' is not a number; the SPICE default stands", ""));
        }
    }
    s.analysis = read_analysis(root.find("analysis"), report.diags);
    if (const json::Value* view = root.find("view");
        view != nullptr && view->is_object()) {
        const json::Object& vo = *view->as_object();
        if (const auto zoom = as_number(vo.find("zoom"))) {
            s.view.zoom = *zoom;
        }
        if (const auto center = as_pos(vo.find("center"))) {
            s.view.center = *center;
        }
        for (const auto& [key, val] : vo) {
            if (key != "zoom" && key != "center") {
                s.view.unknown.set(key, val);
            }
        }
    }

    std::vector<std::uint32_t> seen_ids;
    {
        std::deque<Part> parts;
        read_array<Part>(root.find("parts"), "part", parts, report.diags,
                         seen_ids,
                         [](const json::Object& o, Part& p,
                            std::vector<Diag>& diags) {
                             const PartReader r = read_part(o, diags);
                             if (!r.ok) {
                                 return false;
                             }
                             p = r.p;
                             return true;
                         });
        s.parts_mut() = std::move(parts);
    }
    {
        std::deque<Junction> junctions;
        read_array<Junction>(root.find("junctions"), "junction", junctions,
                             report.diags, seen_ids,
                             [](const json::Object& o, Junction& j,
                                std::vector<Diag>& diags) {
                                 const auto id = as_integer(o.find("id"));
                                 if (!id.has_value() || *id <= 0 ||
                                     *id > 0xFFFFFFFFLL) {
                                     diags.push_back(load_diag(
                                         "circsim.format.bad_id",
                                         "junction without a usable id is dropped",
                                         ""));
                                     return false;
                                 }
                                 j.id = static_cast<std::uint32_t>(*id);
                                 if (const auto pos = as_pos(o.find("pos"))) {
                                     j.pos = *pos;
                                 }
                                 for (const auto& [key, val] : o) {
                                     if (key != "id" && key != "pos") {
                                         j.unknown.set(key, val);
                                     }
                                 }
                                 return true;
                             });
        s.junctions_mut() = std::move(junctions);
    }
    {
        std::deque<Wire> wires;
        read_array<Wire>(root.find("wires"), "wire", wires, report.diags,
                         seen_ids, read_wire);
        s.wires_mut() = std::move(wires);
    }
    {
        std::deque<Label> labels;
        read_array<Label>(root.find("labels"), "label", labels, report.diags,
                          seen_ids,
                          [](const json::Object& o, Label& l,
                             std::vector<Diag>& diags) {
                              const auto id = as_integer(o.find("id"));
                              if (!id.has_value() || *id <= 0 ||
                                  *id > 0xFFFFFFFFLL) {
                                  diags.push_back(load_diag(
                                      "circsim.format.bad_id",
                                      "label without a usable id is dropped",
                                      ""));
                                  return false;
                              }
                              l.id = static_cast<std::uint32_t>(*id);
                              if (const auto name = as_text(o.find("name"))) {
                                  l.name = *name;
                              } else {
                                  diags.push_back(load_diag(
                                      "circsim.format.bad_field",
                                      "label " + std::to_string(l.id) +
                                          ": 'name' is missing or not text",
                                      "label " + std::to_string(l.id)));
                              }
                              if (const auto pos = as_pos(o.find("pos"))) {
                                  l.pos = *pos;
                              }
                              for (const auto& [key, val] : o) {
                                  if (key != "id" && key != "name" &&
                                      key != "pos") {
                                      l.unknown.set(key, val);
                                  }
                              }
                              return true;
                          });
        s.labels_mut() = std::move(labels);
    }

    // Dangling references: wires into parts/junctions that do not exist.
    std::deque<Wire>& wires = s.wires_mut();
    for (std::size_t i = 0; i < wires.size();) {
        const auto endpoint_ok = [&](const WireEndpoint& ep) {
            if (const PinRef* pin = std::get_if<PinRef>(&ep)) {
                return s.find_part(pin->part) != nullptr;
            }
            return s.find_junction(std::get<JunctionRef>(ep).junction) !=
                   nullptr;
        };
        if (endpoint_ok(wires[i].a) && endpoint_ok(wires[i].b)) {
            ++i;
            continue;
        }
        report.diags.push_back(load_diag(
            "circsim.format.dangling_ref",
            "wire " + std::to_string(wires[i].id) +
                " references a missing part or junction; dropped",
            "wire " + std::to_string(wires[i].id)));
        wires.erase(wires.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // next_id: the file's value if sane, else max seen + 1; always above
    // every id this load has seen (dropped duplicates included).
    std::uint32_t next = 1;
    if (const auto n = as_integer(root.find("next_id"))) {
        if (*n > 0 && *n <= 0xFFFFFFFFLL) {
            next = static_cast<std::uint32_t>(*n);
        }
    }
    for (std::uint32_t id : seen_ids) {
        next = std::max(next, id + 1);
    }
    for (const Part& p : s.parts()) {
        next = std::max(next, p.id + 1);
    }
    s.set_next_id(next);
    s.bump();

    for (const auto& [key, val] : root) {
        if (key != "format" && key != "version" && key != "grid" &&
            key != "next_id" && key != "parts" && key != "junctions" &&
            key != "wires" && key != "labels" && key != "analysis" &&
            key != "directives" && key != "temp" && key != "view") {
            s.unknown.set(key, val);
        }
    }
    return s;
}

std::string save_csim(const Schematic& s) {
    json::Object root;
    root.set("format", json::Value::text(kCsimFormat));
    root.set("version", json::Value::integer(kCsimVersion));
    root.set("grid", json::Value::real(s.grid));
    root.set("next_id", json::Value::integer(s.next_id()));

    const auto write_part = [&](const Part& p) {
        json::Object o;
        o.set("id", json::Value::integer(p.id));
        o.set("symbol", json::Value::text(p.symbol));
        o.set("ref", json::Value::text(p.ref));
        o.set("value", json::Value::text(p.value));
        o.set("pos", pos_value(p.pos));
        o.set("rot", json::Value::integer(p.rot));
        o.set("mirror", json::Value::boolean(p.mirror));
        if (!p.params.empty()) {
            json::Object params;
            for (const auto& [key, val] : p.params) {
                params.set(key, json::Value::text(val));
            }
            o.set("params", json::Value::object(std::move(params)));
        }
        for (const auto& [key, val] : p.unknown) {
            o.set(key, val);
        }
        return json::Value::object(std::move(o));
    };
    const auto write_junction = [&](const Junction& j) {
        json::Object o;
        o.set("id", json::Value::integer(j.id));
        o.set("pos", pos_value(j.pos));
        for (const auto& [key, val] : j.unknown) {
            o.set(key, val);
        }
        return json::Value::object(std::move(o));
    };
    const auto write_wire = [&](const Wire& w) {
        json::Object o;
        o.set("id", json::Value::integer(w.id));
        const auto endpoint = [](const WireEndpoint& ep) {
            json::Object e;
            if (const PinRef* pin = std::get_if<PinRef>(&ep)) {
                json::Object p;
                p.set("part", json::Value::integer(pin->part));
                p.set("pin", json::Value::integer(pin->pin));
                e.set("pin", json::Value::object(std::move(p)));
            } else {
                json::Object j;
                j.set("id",
                      json::Value::integer(
                          std::get<JunctionRef>(ep).junction));
                e.set("junction", json::Value::object(std::move(j)));
            }
            return json::Value::object(std::move(e));
        };
        o.set("a", endpoint(w.a));
        o.set("b", endpoint(w.b));
        if (!w.waypoints.empty()) {
            json::Array wps;
            for (const Pos& p : w.waypoints) {
                wps.push_back(pos_value(p));
            }
            o.set("waypoints", json::Value::array(std::move(wps)));
        }
        o.set("ortho", json::Value::boolean(w.ortho));
        for (const auto& [key, val] : w.unknown) {
            o.set(key, val);
        }
        return json::Value::object(std::move(o));
    };
    const auto write_label = [&](const Label& l) {
        json::Object o;
        o.set("id", json::Value::integer(l.id));
        o.set("pos", pos_value(l.pos));
        o.set("name", json::Value::text(l.name));
        for (const auto& [key, val] : l.unknown) {
            o.set(key, val);
        }
        return json::Value::object(std::move(o));
    };

    // Arrays sorted by id (stable diffs); the model vectors may hold any
    // order, so sort copies of the pointers.
    const auto sorted = [](const auto& v) {
        std::vector<const typename std::remove_reference_t<
            decltype(v)>::value_type*> ptrs;
        for (const auto& e : v) {
            ptrs.push_back(&e);
        }
        std::sort(ptrs.begin(), ptrs.end(),
                  [](const auto* a, const auto* b) { return a->id < b->id; });
        return ptrs;
    };

    json::Array parts;
    for (const Part* p : sorted(s.parts())) {
        parts.push_back(write_part(*p));
    }
    root.set("parts", json::Value::array(std::move(parts)));
    json::Array junctions;
    for (const Junction* j : sorted(s.junctions())) {
        junctions.push_back(write_junction(*j));
    }
    root.set("junctions", json::Value::array(std::move(junctions)));
    json::Array wires;
    for (const Wire* w : sorted(s.wires())) {
        wires.push_back(write_wire(*w));
    }
    root.set("wires", json::Value::array(std::move(wires)));
    json::Array labels;
    for (const Label* l : sorted(s.labels())) {
        labels.push_back(write_label(*l));
    }
    root.set("labels", json::Value::array(std::move(labels)));

    write_analysis(root, s.analysis);
    root.set("directives", json::Value::text(s.directives));
    // The run temperature: written only when set, so an unset temp keeps
    // the file byte-identical to what older writers produced.
    if (s.temp.has_value()) {
        root.set("temp", json::Value::real(*s.temp));
    }

    json::Object view;
    view.set("zoom", json::Value::real(s.view.zoom));
    view.set("center", pos_value(s.view.center));
    for (const auto& [key, val] : s.view.unknown) {
        view.set(key, val);
    }
    root.set("view", json::Value::object(std::move(view)));

    for (const auto& [key, val] : s.unknown) {
        root.set(key, val);
    }
    return json::write(json::Value::object(std::move(root)));
}

ws::core::Result<Schematic> read_csim(const ws::settings::FileSystem& fs,
                                      std::string_view path,
                                      LoadReport& report) {
    const auto text = fs.read(path);
    if (!text) {
        return ws::core::unexpected<ws::core::Error>(
            ws::core::make_error(2, "circsim",
                                 "cannot read '" + std::string(path) +
                                     "': " + text.error().message));
    }
    return load_csim(*text, report);
}

ws::core::Status write_csim(ws::settings::FileSystem& fs,
                            std::string_view path, const Schematic& s) {
    return fs.write(path, save_csim(s));
}

}  // namespace ws::circsim
