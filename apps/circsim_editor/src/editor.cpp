// apps/circsim_editor - the shared core's implementation (see
// editor.hpp). The pure geometry lives here so the panels and the
// commands share one truth: snapping, bounds, hit-testing, the ortho
// route, the symbol renderer and the cache refreshers.

#include "editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <ws/circsim/erc.hpp>

namespace editor {
namespace {

using canvas::Paint;
using canvas::Point;
using ws::circsim::JunctionRef;
using ws::circsim::PinRef;

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

// Squared distance from `p` to segment ab.
[[nodiscard]] double dist_to_segment(Pos a, Pos b, Pos p) {
    const Pos ab{b.x - a.x, b.y - a.y};
    const Pos ap{p.x - a.x, p.y - a.y};
    const double denom = ab.x * ab.x + ab.y * ab.y;
    if (denom <= 0.0) {
        return len(ap);
    }
    double t = (ap.x * ab.x + ap.y * ab.y) / denom;
    t = std::max(0.0, std::min(1.0, t));
    return len(Pos{ap.x - t * ab.x, ap.y - t * ab.y});
}

}  // namespace

const ws::circsim::SymbolDef* Editor::sym_of(
    const ws::circsim::Part& p) const {
    return library.find(p.symbol);
}

Pos Editor::snap(Pos world) const {
    const ws::circsim::Schematic* s = schem();
    const double grid = s != nullptr && s->grid > 0.0 ? s->grid : 1.0;
    return {std::round(world.x / grid) * grid,
            std::round(world.y / grid) * grid};
}

std::optional<Bounds> Editor::part_bounds(std::uint32_t id) const {
    const ws::circsim::Schematic* s = schem();
    const ws::circsim::Part* p = s != nullptr ? s->find_part(id) : nullptr;
    const ws::circsim::SymbolDef* sym =
        p != nullptr ? sym_of(*p) : nullptr;
    if (p == nullptr) {
        return std::nullopt;
    }
    if (sym == nullptr) {
        return Bounds{p->pos.x - 1.0, p->pos.y - 1.0, p->pos.x + 1.0,
                      p->pos.y + 1.0};
    }
    const Bounds local = ws::circsim::symbol_bounds(*sym);
    std::optional<Bounds> out;
    const auto grow = [&](Pos w) {
        if (!out.has_value()) {
            out = Bounds{w.x, w.y, w.x, w.y};
            return;
        }
        out->min_x = std::min(out->min_x, w.x);
        out->min_y = std::min(out->min_y, w.y);
        out->max_x = std::max(out->max_x, w.x);
        out->max_y = std::max(out->max_y, w.y);
    };
    for (const Pos corner :
         {Pos{local.min_x, local.min_y}, Pos{local.max_x, local.min_y},
          Pos{local.min_x, local.max_y}, Pos{local.max_x, local.max_y}}) {
        grow(ws::circsim::transform_pos(*p, corner));
    }
    return out;
}

std::vector<Pos> Editor::wire_path(const ws::circsim::Wire& w) const {
    std::vector<Pos> out;
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return out;
    }
    const auto endpoint_pos = [&](const ws::circsim::WireEndpoint& end)
        -> std::optional<Pos> {
        if (const PinRef* pin = std::get_if<PinRef>(&end)) {
            const ws::circsim::Part* p = s->find_part(pin->part);
            const ws::circsim::SymbolDef* sym =
                p != nullptr ? sym_of(*p) : nullptr;
            if (p != nullptr && sym != nullptr &&
                pin->pin < sym->pins.size()) {
                return ws::circsim::pin_pos(*sym, *p, pin->pin);
            }
            return std::nullopt;
        }
        const JunctionRef* j = std::get_if<JunctionRef>(&end);
        const ws::circsim::Junction* found =
            j != nullptr ? s->find_junction(j->junction) : nullptr;
        if (found != nullptr) {
            return found->pos;
        }
        return std::nullopt;
    };
    const std::optional<Pos> a = endpoint_pos(w.a);
    const std::optional<Pos> b = endpoint_pos(w.b);
    if (!a.has_value() || !b.has_value()) {
        return out;  // dangling: the loader's tolerance, nothing to draw
    }
    out.reserve(w.waypoints.size() + 2);
    out.push_back(*a);
    for (const Pos& p : w.waypoints) {
        out.push_back(p);
    }
    out.push_back(*b);
    return out;
}

std::vector<Pos> Editor::route_wire(Pos a, int a_dir, Pos b, int b_dir) {
    // The lead length: how far a route commits to a direction before
    // it may turn. One grid unit reads clean at every zoom (a pin's
    // own lead is typically three).
    constexpr double kLead = 1.0;
    const auto unit = [](int dir) -> Pos {
        switch (((dir % 360) + 360) % 360) {
            case 0: return {1.0, 0.0};
            case 90: return {0.0, 1.0};
            case 180: return {-1.0, 0.0};
            default: return {0.0, -1.0};  // 270
        }
    };

    // Full polyline a -> ... -> b; the stored waypoints are its
    // interior after folding.
    std::vector<Pos> full;
    full.push_back(a);
    Pos from = a;
    if (a_dir >= 0) {
        const Pos u = unit(a_dir);
        from = {a.x + u.x * kLead, a.y + u.y * kLead};
        full.push_back(from);
    }
    Pos target = b;
    if (b_dir >= 0) {
        const Pos u = unit(b_dir);
        // Approach b ALONG b_dir: stop one lead short of it.
        target = {b.x - u.x * kLead, b.y - u.y * kLead};
    }
    if (!eps_eq(from.x, target.x, 1e-9) && !eps_eq(from.y, target.y, 1e-9)) {
        // One corner (an L): the leg out of `from` continues the
        // committed direction (horizontal when free), the leg into
        // `target` closes. The final approach leg target -> b already
        // runs along b_dir, so one corner always suffices.
        const bool start_horizontal =
            a_dir == 0 || a_dir == 180 || a_dir < 0;
        if (start_horizontal) {
            full.push_back(Pos{target.x, from.y});
        } else {
            full.push_back(Pos{from.x, target.y});
        }
    }
    if (b_dir >= 0) {
        full.push_back(target);
    }
    full.push_back(b);

    // Fold duplicates and collinear middles (these become the stored
    // waypoints; noise would ride straight into the .csim).
    std::vector<Pos> folded;
    for (const Pos& p : full) {
        if (!folded.empty() && eps_eq(folded.back().x, p.x, 1e-9) &&
            eps_eq(folded.back().y, p.y, 1e-9)) {
            continue;
        }
        if (folded.size() >= 2u) {
            const Pos& p0 = folded[folded.size() - 2u];
            const Pos& p1 = folded[folded.size() - 1u];
            if ((eps_eq(p0.x, p1.x, 1e-9) && eps_eq(p1.x, p.x, 1e-9)) ||
                (eps_eq(p0.y, p1.y, 1e-9) && eps_eq(p1.y, p.y, 1e-9))) {
                folded.back() = p;  // collinear: extend the leg
                continue;
            }
        }
        folded.push_back(p);
    }
    if (folded.size() < 2u) {
        return {};
    }
    return {folded.begin() + 1, folded.end() - 1};
}

std::optional<PinHit> Editor::hit_pin(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<PinHit> best;
    double best_d = radius;
    for (const ws::circsim::Part& p : s->parts()) {
        const ws::circsim::SymbolDef* sym = sym_of(p);
        if (sym == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < sym->pins.size(); ++i) {
            const Pos at = ws::circsim::pin_pos(*sym, p,
                                                static_cast<std::uint32_t>(i));
            const double d = len(Pos{at.x - world.x, at.y - world.y});
            if (d <= best_d) {
                best_d = d;
                best = PinHit{PinRef{p.id, static_cast<std::uint32_t>(i)}, at,
                              sym->pins[i].dir};
            }
        }
    }
    return best;
}

std::optional<std::uint32_t> Editor::hit_junction(Pos world,
                                                  double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> best;
    double best_d = radius;
    for (const ws::circsim::Junction& j : s->junctions()) {
        const double d = len(Pos{j.pos.x - world.x, j.pos.y - world.y});
        if (d <= best_d) {
            best_d = d;
            best = j.id;
        }
    }
    return best;
}

Selection Editor::hit_any(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return {};
    }
    // Parts first (they are the tallest objects), then pins? No: a
    // click that lands on a pin should select the PART (pins are the
    // part's), so parts-before-pins is not a question here. Junctions
    // beat wires (a dot sits on every wire end), wires beat labels.
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value() && world.x >= b->min_x && world.x <= b->max_x &&
            world.y >= b->min_y && world.y <= b->max_y) {
            return {Selection::Kind::Part, p.id};
        }
    }
    if (const std::uint32_t j = hit_junction(world, radius).value_or(0)) {
        return {Selection::Kind::Junction, j};
    }
    double best_d = radius;
    for (const ws::circsim::Wire& w : s->wires()) {
        const std::vector<Pos> path = wire_path(w);
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double d = dist_to_segment(path[i - 1], path[i], world);
            if (d <= best_d) {
                best_d = d;
                return {Selection::Kind::Wire, w.id};
            }
        }
    }
    for (const ws::circsim::Label& l : s->labels()) {
        if (len(Pos{l.pos.x - world.x, l.pos.y - world.y}) <= radius + 1.0) {
            return {Selection::Kind::Label, l.id};
        }
    }
    return {};
}

std::optional<Bounds> Editor::content_bounds() const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::optional<Bounds> out;
    const auto grow = [&](Pos w) {
        if (!out.has_value()) {
            out = Bounds{w.x, w.y, w.x, w.y};
            return;
        }
        out->min_x = std::min(out->min_x, w.x);
        out->min_y = std::min(out->min_y, w.y);
        out->max_x = std::max(out->max_x, w.x);
        out->max_y = std::max(out->max_y, w.y);
    };
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value()) {
            grow(Pos{b->min_x, b->min_y});
            grow(Pos{b->max_x, b->max_y});
        }
    }
    for (const ws::circsim::Junction& j : s->junctions()) {
        grow(j.pos);
    }
    for (const ws::circsim::Label& l : s->labels()) {
        grow(l.pos);
    }
    for (const ws::circsim::Wire& w : s->wires()) {
        for (const Pos& p : wire_path(w)) {
            grow(p);
        }
    }
    return out;
}

void Editor::jump_to(const ws::circsim::DiagTarget& t) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    switch (t.kind) {
        case ws::circsim::DiagTarget::Kind::Part:
            sel = {Selection::Kind::Part, t.part};
            break;
        case ws::circsim::DiagTarget::Kind::Wire:
            sel = {Selection::Kind::Wire, t.obj};
            break;
        case ws::circsim::DiagTarget::Kind::Label:
            sel = {Selection::Kind::Label, t.obj};
            break;
        case ws::circsim::DiagTarget::Kind::Net:
        case ws::circsim::DiagTarget::Kind::Directives:
        case ws::circsim::DiagTarget::Kind::Analysis:
        case ws::circsim::DiagTarget::Kind::None:
            break;
    }
    const std::optional<Bounds> b = ws::circsim::focus_bounds(
        t, *s, problems.nets_valid ? &problems.nets : nullptr, &library);
    if (b.has_value()) {
        view.fit(*b, 3.0);
    }
    if (t.kind == ws::circsim::DiagTarget::Kind::Net) {
        const ws::circsim::Net* n = nullptr;
        for (const ws::circsim::Net& candidate : problems.nets.nets) {
            if (candidate.id == t.net) {
                n = &candidate;
                break;
            }
        }
        if (n != nullptr) {
            status("net " + n->name);
        }
    }
}

void Editor::refresh_problems(bool from_run,
                              std::vector<ws::circsim::Diag> diags,
                              std::string header) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    problems.has = true;
    problems.from_run = from_run;
    problems.version = s->version();
    problems.header = std::move(header);
    problems.diags = std::move(diags);
    problems.lines.clear();
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const ws::circsim::Diag& d : problems.diags) {
        problems.lines.push_back(d.render());
        if (d.severity == ws::circsim::Severity::Error) {
            ++errors;
        } else if (d.severity == ws::circsim::Severity::Warning) {
            ++warnings;
        }
    }
    char tail[64];
    std::snprintf(tail, sizeof tail, " - %zu error%s, %zu warning%s",
                  errors, errors == 1 ? "" : "s", warnings,
                  warnings == 1 ? "" : "s");
    problems.header += tail;
}

void Editor::run_erc() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    const ws::circsim::NetBuild build =
        ws::circsim::build_nets(*s, &library);
    problems.nets = build.nets;
    problems.nets_valid = true;
    problems.map_valid = false;
    refresh_problems(false, ws::circsim::run_erc(*s, build, &library),
                     "erc");
}

void Editor::refresh_netlist() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    const ws::circsim::NetBuild build =
        ws::circsim::build_nets(*s, &library);
    const auto deck =
        ws::circsim::export_deck(*s, build.nets, nullptr, nullptr, &library);
    if (!deck.has_value()) {
        netlist.has = false;
        netlist.lines.clear();
        toast("netlist export failed: " + deck.error().message,
              gui::ToastSeverity::error);
        return;
    }
    netlist.has = true;
    netlist.version = s->version();
    netlist.text = deck->text;
    netlist.lines.clear();
    std::size_t at = 0;
    while (at <= netlist.text.size()) {
        const std::size_t nl = netlist.text.find('\n', at);
        netlist.lines.push_back(netlist.text.substr(
            at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) {
            break;
        }
        at = nl + 1;
    }
}

void Editor::document_changed() {
    tool = Tool::Select;
    place_symbol.clear();
    ghost_rot = 0;
    ghost_mirror = false;
    sel.clear();
    drag = Drag{};
    wire_draft = WireDraft{};
    problems = ProblemsCache{};
    netlist = NetlistCache{};
    results = ResultsCache{};
    const ws::circsim::Schematic* s = schem();
    if (s != nullptr) {
        view.zoom = s->view.zoom > 0.0 ? s->view.zoom : 26.0;
        view.center = s->view.center;
    } else {
        view.zoom = 26.0;
        view.center = {0.0, 0.0};
    }
}

void Editor::toast(std::string text, gui::ToastSeverity severity) const {
    if (toasts != nullptr) {
        // The caller's string must outlive the toast's borrow: this
        // holder is mutable-through-const exactly for that (the
        // shell_example's message_ precedent).
        static std::string holder;
        holder = std::move(text);
        toasts->push({holder.c_str(), severity, nullptr, 4.0f});
    }
}

void Editor::status(std::string text) const {
    if (set_status) {
        set_status(std::move(text));
    }
}

// --- the symbol renderer ------------------------------------------------------

void draw_symbol(canvas::Canvas2D& c, const ws::circsim::SymbolDef& sym,
                 Pos pos, int rot, bool mirror, const Paint& body,
                 std::uint32_t text_color, const ws::ui::r2d::Font* font) {
    // The placement is a Part-shaped transform (transform_pos's exact
    // math - mirror, then rotation, then translation - so the picture
    // and the pins can never disagree).
    ws::circsim::Part placement{};
    placement.pos = pos;
    placement.rot = rot;
    placement.mirror = mirror;
    const auto place = [&](Pos local) {
        const Pos w = ws::circsim::transform_pos(placement, local);
        return canvas::Point{static_cast<float>(w.x),
                             static_cast<float>(w.y)};
    };

    // Does any draw primitive vertex sit on this pin? (The built-in
    // tables carry their own leads; the stub below is the .csym
    // safety net for symbols whose prims fall short of their pins.)
    const auto prim_touches = [&](const Pos& pin) {
        for (const ws::circsim::DrawPrim& d : sym.draw) {
            for (const Pos& v : d.pts) {
                if (eps_eq(v.x, pin.x, 0.25) && eps_eq(v.y, pin.y, 0.25)) {
                    return true;
                }
            }
        }
        return false;
    };

    for (const ws::circsim::DrawPrim& d : sym.draw) {
        switch (d.kind) {
            case ws::circsim::DrawPrim::Kind::Line:
                if (d.pts.size() >= 2u) {
                    c.stroke_line(place(d.pts[0]), place(d.pts[1]), body);
                }
                break;
            case ws::circsim::DrawPrim::Kind::Poly: {
                std::vector<canvas::Point> pts;
                pts.reserve(d.pts.size());
                for (const Pos& p : d.pts) {
                    pts.push_back(place(p));
                }
                if (pts.size() >= 2u) {
                    c.stroke_polyline(pts.data(), static_cast<int>(pts.size()),
                                      body, true);
                }
                break;
            }
            case ws::circsim::DrawPrim::Kind::Rect: {
                if (d.pts.size() >= 2u) {
                    const canvas::Point a = place(d.pts[0]);
                    const canvas::Point b = place(d.pts[1]);
                    // A closed quad, not an axis-aligned rect call: the
                    // placement may have rotated the box.
                    const canvas::Point quad[4] = {
                        a, {b.x, a.y}, b, {a.x, b.y}};
                    c.stroke_polyline(quad, 4, body, true);
                }
                break;
            }
            case ws::circsim::DrawPrim::Kind::Circle:
                if (!d.pts.empty()) {
                    c.stroke_circle(place(d.pts[0]),
                                    static_cast<float>(d.r), body);
                }
                break;
            case ws::circsim::DrawPrim::Kind::Arc: {
                if (d.pts.empty()) {
                    break;
                }
                // Mirror flips an arc to its reflection (angles
                // negated about the y axis, sweep swapped); rotation
                // adds to both ends. Both in degrees here, radians at
                // the canvas seam.
                double a0 = d.a0;
                double a1 = d.a1;
                if (mirror) {
                    a0 = 180.0 - d.a1;
                    a1 = 180.0 - d.a0;
                }
                const int add = ((rot % 360) + 360) % 360;
                a0 += static_cast<double>(add);
                a1 += static_cast<double>(add);
                constexpr double kRad = 3.14159265358979323846 / 180.0;
                c.arc(place(d.pts[0]), static_cast<float>(d.r),
                      static_cast<float>(d.r), static_cast<float>(a0 * kRad),
                      static_cast<float>(a1 * kRad));
                c.stroke(body);
                break;
            }
            case ws::circsim::DrawPrim::Kind::Dot:
                if (!d.pts.empty()) {
                    c.fill_circle(place(d.pts[0]), static_cast<float>(d.r),
                                  body.color);
                }
                break;
            case ws::circsim::DrawPrim::Kind::Text:
                if (!d.pts.empty() && font != nullptr && !d.text.empty()) {
                    const canvas::Point at = place(d.pts[0]);
                    (void)c.draw_text(at.x, at.y, d.text.c_str(), *font,
                                      text_color);
                }
                break;
        }
    }

    // The pin-lead safety net: a pin nothing touches gets a stub
    // pointing into the body (opposite the pin's own direction).
    const Bounds b = ws::circsim::symbol_bounds(sym);
    for (const ws::circsim::PinDef& pin : sym.pins) {
        const Pos local{pin.dx, pin.dy};
        if (prim_touches(local)) {
            continue;
        }
        const Pos u = [&] {
            switch ((((pin.dir + 180) % 360) + 360) % 360) {
                case 0: return Pos{1.0, 0.0};
                case 90: return Pos{0.0, 1.0};
                case 180: return Pos{-1.0, 0.0};
                default: return Pos{0.0, -1.0};
            }
        }();
        // Until the bounds' edge (plus a little overlap), one unit
        // minimum, three maximum - a lead, not a transmission line.
        const bool outside_x =
            local.x < b.min_x || local.x > b.max_x;
        const bool outside_y =
            local.y < b.min_y || local.y > b.max_y;
        double reach = 1.0;
        if (outside_x) {
            reach = std::fabs(u.x) > 0.0
                        ? std::fabs(local.x - (u.x > 0.0 ? b.max_x : b.min_x))
                        : 1.0;
        } else if (outside_y) {
            reach = std::fabs(u.y) > 0.0
                        ? std::fabs(local.y - (u.y > 0.0 ? b.max_y : b.min_y))
                        : 1.0;
        }
        reach = std::max(1.0, std::min(3.0, reach + 0.5));
        c.stroke_line(place(local),
                      place(Pos{local.x + u.x * reach,
                                local.y + u.y * reach}),
                      body);
    }
}

}  // namespace editor
