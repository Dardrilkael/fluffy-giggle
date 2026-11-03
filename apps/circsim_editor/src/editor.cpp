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

canvas::GridSpec Editor::grid_spec() const {
    // The schematic's own grid is the base; everything else (faces,
    // thresholds, cap) is the spec's stock, which the panel paints
    // over with the theme's tokens.
    canvas::GridSpec spec;
    const ws::circsim::Schematic* s = schem();
    spec.base = s != nullptr && s->grid > 0.0 ? s->grid : 1.0;
    spec.major_interval = 5;  // the x5 ladder the draw assumes
    return spec;
}

Pos Editor::snap(Pos world, bool coarse) const {
    // snap_coord reads the SAME spec the grid renderer draws (the
    // drawn dots and the snap targets can never disagree).
    const canvas::GridSpec spec = grid_spec();
    return {canvas::snap_coord(world.x, spec, coarse),
            canvas::snap_coord(world.y, spec, coarse)};
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
    // The heading of an axis-aligned leg (0/90/180/270), -1 for a
    // zero-length leg, -2 for a diagonal.
    const auto heading = [](Pos p, Pos q) -> int {
        if (eps_eq(p.x, q.x, 1e-9) && eps_eq(p.y, q.y, 1e-9)) {
            return -1;
        }
        if (eps_eq(p.y, q.y, 1e-9)) {
            return q.x > p.x ? 0 : 180;
        }
        if (eps_eq(p.x, q.x, 1e-9)) {
            return q.y > p.y ? 90 : 270;
        }
        return -2;
    };

    // A pin's dir points OUT of its part. A route leaves `a` along
    // a_dir (one lead out), and reaches `b` from OUTSIDE: its last
    // lead sits one unit out along b_dir and the final leg runs back
    // in toward the pin (never across the symbol body).
    Pos from = a;
    if (a_dir >= 0) {
        const Pos u = unit(a_dir);
        from = {a.x + u.x * kLead, a.y + u.y * kLead};
    }
    Pos target = b;
    if (b_dir >= 0) {
        const Pos u = unit(b_dir);
        target = {b.x + u.x * kLead, b.y + u.y * kLead};
    }

    const auto assemble = [&](const std::vector<Pos>& mid) {
        std::vector<Pos> full;
        full.push_back(a);
        if (a_dir >= 0) {
            full.push_back(from);
        }
        for (const Pos& m : mid) {
            full.push_back(m);
        }
        if (b_dir >= 0) {
            full.push_back(target);
        }
        full.push_back(b);
        std::vector<Pos> dedup;  // drop zero-length legs
        for (const Pos& p : full) {
            if (dedup.empty() || heading(dedup.back(), p) != -1) {
                dedup.push_back(p);
            }
        }
        return dedup;
    };

    // Valid: every leg axis-aligned, no leg reverses the one before
    // it, the first leg is a_dir and the last arrives against b_dir.
    const auto valid = [&](const std::vector<Pos>& path) {
        if (path.size() < 2u) {
            return false;
        }
        int prev = -1;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const int h = heading(path[i], path[i + 1]);
            if (h < 0) {
                return false;
            }
            if (i == 0 && a_dir >= 0 && h != ((a_dir % 360) + 360) % 360) {
                return false;
            }
            if (prev >= 0 && (prev + 180) % 360 == h) {
                return false;
            }
            prev = h;
        }
        return b_dir < 0 || prev == (((b_dir % 360) + 360) % 360 + 180) % 360;
    };

    // Candidate middles between `from` and `target`, simplest first:
    // straight, the two Ls (the one continuing a_dir first), the
    // midpoint Zs, then the U detours that go one lead past both ends
    // (what a pin facing away from its partner needs).
    const double mx = std::floor((from.x + target.x) * 0.5);
    const double my = std::floor((from.y + target.y) * 0.5);
    const double hi_x = std::max(from.x, target.x) + kLead;
    const double lo_x = std::min(from.x, target.x) - kLead;
    const double hi_y = std::max(from.y, target.y) + kLead;
    const double lo_y = std::min(from.y, target.y) - kLead;
    const std::vector<Pos> l_h{Pos{target.x, from.y}};
    const std::vector<Pos> l_v{Pos{from.x, target.y}};
    const bool vertical_first = a_dir == 90 || a_dir == 270;
    std::vector<std::vector<Pos>> candidates;
    candidates.push_back({});
    candidates.push_back(vertical_first ? l_v : l_h);
    candidates.push_back(vertical_first ? l_h : l_v);
    candidates.push_back({Pos{mx, from.y}, Pos{mx, target.y}});
    candidates.push_back({Pos{from.x, my}, Pos{target.x, my}});
    for (const double ox : {hi_x, lo_x}) {
        candidates.push_back({Pos{ox, from.y}, Pos{ox, target.y}});
    }
    for (const double oy : {hi_y, lo_y}) {
        candidates.push_back({Pos{from.x, oy}, Pos{target.x, oy}});
    }

    std::vector<Pos> best;
    std::size_t best_bends = 0;
    double best_len = 0.0;
    for (const std::vector<Pos>& mid : candidates) {
        const std::vector<Pos> path = assemble(mid);
        if (!valid(path)) {
            continue;
        }
        // Score: corners first (collinear middles do not count), then
        // length. Strict < keeps the earlier (simpler) candidate on ties.
        std::size_t bends = 0;
        double len = 0.0;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            len += std::fabs(path[i + 1].x - path[i].x) +
                   std::fabs(path[i + 1].y - path[i].y);
            if (i + 2 < path.size() &&
                heading(path[i], path[i + 1]) !=
                    heading(path[i + 1], path[i + 2])) {
                ++bends;
            }
        }
        if (best.empty() || bends < best_bends ||
            (bends == best_bends && len < best_len - 1e-9)) {
            best = path;
            best_bends = bends;
            best_len = len;
        }
    }
    if (best.empty()) {
        // No clean shape (e.g. a == b): a bare a -> b, no waypoints.
        return {};
    }

    // Fold collinear middles (these become the stored waypoints; noise
    // would ride straight into the .csim).
    std::vector<Pos> folded;
    for (const Pos& p : best) {
        if (folded.size() >= 2u &&
            heading(folded[folded.size() - 2u], folded.back()) ==
                heading(folded.back(), p)) {
            folded.back() = p;
            continue;
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
                // The WORLD direction: the symbol's local pin dir pushed
                // through the part's mirror/rotation, so a wire leaves a
                // rotated or mirrored part the way its pin really faces.
                best = PinHit{PinRef{p.id, static_cast<std::uint32_t>(i)}, at,
                              ws::circsim::pin_dir(
                                  *sym, p, static_cast<std::uint32_t>(i))};
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

const std::vector<Pos>& Editor::auto_junction_dots() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        join_dots_.clear();
        joins_valid_ = false;
        return join_dots_;
    }
    if (!joins_valid_ || joins_version_ != s->version()) {
        // The same seam ERC and the export read: one build_nets per
        // edit (millisecond work at editor scales), never per frame.
        const ws::circsim::NetBuild build =
            ws::circsim::build_nets(*s, &library);
        join_dots_.clear();
        for (const ws::circsim::SilentJoin& j : build.silent_joins) {
            join_dots_.push_back(j.pos);
        }
        joins_version_ = s->version();
        joins_valid_ = true;
    }
    return join_dots_;
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
    join_dots_.clear();
    joins_valid_ = false;
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
    // The geometry lives in ws::circsim's render seam (render_symbol):
    // one pure flattening shared with the CLI's SVG dump and the tests.
    // This is the painting half only - strokes to canvas calls.
    ws::circsim::Part placement{};
    placement.pos = pos;
    placement.rot = rot;
    placement.mirror = mirror;
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    for (const ws::circsim::SymbolStroke& s :
         ws::circsim::render_symbol(sym, placement)) {
        const auto pt = [&](std::size_t i) {
            return Point{static_cast<float>(s.pts[i].x),
                         static_cast<float>(s.pts[i].y)};
        };
        switch (s.kind) {
            case ws::circsim::SymbolStroke::Kind::Line:
                c.stroke_line(pt(0), pt(1), body);
                break;
            case ws::circsim::SymbolStroke::Kind::Poly: {
                std::vector<Point> pts;
                pts.reserve(s.pts.size());
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    pts.push_back(pt(i));
                }
                c.stroke_polyline(pts.data(),
                                  static_cast<int>(pts.size()), body,
                                  s.closed);
                break;
            }
            case ws::circsim::SymbolStroke::Kind::PolyFill: {
                std::vector<Point> pts;
                pts.reserve(s.pts.size());
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    pts.push_back(pt(i));
                }
                if (pts.size() >= 3u) {
                    c.fill_polygon(pts.data(),
                                   static_cast<int>(pts.size()), body.color);
                }
                break;
            }
            case ws::circsim::SymbolStroke::Kind::Circle:
                c.stroke_circle(pt(0), static_cast<float>(s.r), body);
                break;
            case ws::circsim::SymbolStroke::Kind::Arc:
                c.arc(pt(0), static_cast<float>(s.r),
                      static_cast<float>(s.r),
                      static_cast<float>(s.a0 * kRad),
                      static_cast<float>(s.a1 * kRad));
                c.stroke(body);
                break;
            case ws::circsim::SymbolStroke::Kind::Dot:
                c.fill_circle(pt(0), static_cast<float>(s.r), body.color);
                break;
            case ws::circsim::SymbolStroke::Kind::Text:
                if (font != nullptr && !s.text.empty()) {
                    (void)c.draw_text(pt(0).x, pt(0).y, s.text.c_str(),
                                      *font, text_color);
                }
                break;
        }
    }
}

}  // namespace editor
