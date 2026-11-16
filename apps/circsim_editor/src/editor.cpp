// apps/circsim_editor - the shared core's implementation (see
// editor.hpp). The pure geometry lives here so the panels and the
// commands share one truth: snapping, bounds, hit-testing, the ortho
// route, the symbol renderer and the cache refreshers.

#include "editor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <cstdio>
#include <span>
#include <string_view>

#include <ws/core/assert.hpp>
#include <ws/circsim/erc.hpp>
#include <ws/circsim/overlay.hpp>

namespace editor {
namespace {

using canvas::Paint;
using canvas::Point;
using ws::circsim::JunctionRef;
using ws::circsim::PinRef;

// The pure geometry the panels and commands share (route/tidy/order,
// refdes, hit tests, bounds, magnetism, the color wheel) lives in
// ws::circsim (geom.hpp - stage 11, the lib extraction); the Editor
// methods below are thin forwarders that keep this app's own API
// unchanged. These locals are the editor-state HALF: what reads the
// camera, the selection or the caches, plus the shared math aliases.
using ws::circsim::BoundsBuilder;
using ws::circsim::dist_to_segment;
using ws::circsim::is_auto_ref;   // annotate's rename guard
using ws::circsim::mentions_token;  // annotate's directive check

[[nodiscard]] double len(Pos p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

// The label pick's extra reach, in screen px beside the pick radius:
// the text's own footprint is wider than its anchor point, so the
// click target grows by a few pixels - never a fixed world unit
// (which at max zoom answered clicks 400px away).
constexpr double kLabelPickPx = 6.0;

constexpr double kPi = 3.14159265358979323846;

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
    if (p == nullptr) {
        return std::nullopt;
    }
    // A new version retires the WHOLE cache, not just the entry being
    // asked for: an edit may have moved, rotated or deleted any part,
    // and an entry computed against an older version must never
    // answer for the new one. (Stamping the new version on a single
    // recomputed entry once left every OTHER part serving stale
    // boxes - the dragged part's highlight and refdes text lagged at
    // the pre-drag position while the symbol itself moved.)
    if (!bounds_valid_ || bounds_version_ != s->version()) {
        part_bounds_cache_.clear();
        bounds_version_ = s->version();
        bounds_valid_ = true;
    }
    const auto cached = part_bounds_cache_.find(id);
    if (cached != part_bounds_cache_.end()) {
        return cached->second;
    }
    // Cache miss: compute through the lib seam and store.
    const std::optional<Bounds> part_box =
        ws::circsim::part_world_bounds(*s, library, id);
    if (part_box.has_value()) {
        part_bounds_cache_[id] = *part_box;
    }
    return part_box;
}

std::vector<Pos> Editor::wire_path(const ws::circsim::Wire& w) const {
    // The lib's own polyline (ws::circsim::wire_path): endpoints
    // through their refs, then the waypoints between.
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) return {};
    return ws::circsim::wire_path(*s, library, w);
}

const WireGeom& Editor::wire_geom(const ws::circsim::Wire& w) const {
    static const WireGeom kNone{};
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) return kNone;
    if (!wire_geom_valid_ || wire_geom_version_ != s->version()) {
        wire_geom_cache_.clear();
        wire_geom_version_ = s->version();
        wire_geom_valid_ = true;
    }
    const auto cached = wire_geom_cache_.find(w.id);
    if (cached != wire_geom_cache_.end()) return cached->second;
    WireGeom g;
    g.path = ws::circsim::wire_path(*s, library, w);
    if (!g.path.empty()) {
        g.min_x = g.max_x = static_cast<double>(g.path[0].x);
        g.min_y = g.max_y = static_cast<double>(g.path[0].y);
        for (const Pos& p : g.path) {
            g.min_x = std::min(g.min_x, static_cast<double>(p.x));
            g.max_x = std::max(g.max_x, static_cast<double>(p.x));
            g.min_y = std::min(g.min_y, static_cast<double>(p.y));
            g.max_y = std::max(g.max_y, static_cast<double>(p.y));
        }
    }
    return wire_geom_cache_.emplace(w.id, std::move(g)).first->second;
}

std::vector<Pos> Editor::route_wire(Pos a, int a_dir, Pos b, int b_dir,
                                    bool flip) {
    return ws::circsim::route_wire(a, a_dir, b, b_dir, flip);
}

std::vector<Pos> Editor::tidy_route(const std::vector<Pos>& path) {
    return ws::circsim::tidy_route(path);
}

std::size_t Editor::tidy_selection() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr || doc == nullptr) {
        return 0;
    }
    // Which wires does the selection cover?
    const auto covered = [&](const ws::circsim::Wire& w) {
        switch (sel.kind) {
            case Selection::Kind::Wire:
                return w.id == sel.id;
            case Selection::Kind::Part: {
                const PinRef* a = std::get_if<PinRef>(&w.a);
                const PinRef* b = std::get_if<PinRef>(&w.b);
                return (a != nullptr && a->part == sel.id) ||
                       (b != nullptr && b->part == sel.id);
            }
            case Selection::Kind::Junction: {
                const JunctionRef* a = std::get_if<JunctionRef>(&w.a);
                const JunctionRef* b = std::get_if<JunctionRef>(&w.b);
                return (a != nullptr && a->junction == sel.id) ||
                       (b != nullptr && b->junction == sel.id);
            }
            case Selection::Kind::Label:
            case Selection::Kind::None:
                return false;
        }
        return false;
    };
    std::vector<WireRoute> routes;
    std::size_t in_scope = 0;
    std::size_t freeform = 0;
    for (const ws::circsim::Wire& w : s->wires()) {
        if (!covered(w)) {
            continue;
        }
        ++in_scope;
        if (!w.ortho) {
            ++freeform;  // the author's shape: only ortho wires tidy
            continue;
        }
        const std::vector<Pos>& path = wire_geom(w).path;
        if (path.size() < 2u) {
            continue;  // dangling: nothing to straighten
        }
        routes.push_back(WireRoute{w.id, tidy_route(path)});
    }
    if (in_scope == 0u) {
        status("nothing to tidy - select a wire, part or junction");
        return 0;
    }
    const std::size_t changed = push_tidy_wires(*doc, std::move(routes));
    if (changed == 0u) {
        status(freeform == in_scope ? "freeform wires are left as drawn"
                                    : "already tidy");
        return 0;
    }
    status("tidied " + std::to_string(changed) +
           (changed == 1u ? " wire" : " wires"));
    if (const std::optional<Bounds> b = selection_bounds()) {
        pulse_at(Pos{(b->min_x + b->max_x) * 0.5, (b->min_y + b->max_y) * 0.5});
    }
    return changed;
}

bool Editor::transform_part(std::uint32_t id, int rot, bool mirror,
                            std::string name) {
    ws::circsim::Schematic* s = schem();
    if (s == nullptr || doc == nullptr) {
        return false;
    }
    ws::circsim::Part* p = s->find_part(id);
    if (p == nullptr) {
        return false;
    }
    const int was_rot = p->rot;
    const bool was_mirror = p->mirror;
    // The part visits the new orientation; the attached wires' endpoints
    // resolve live through their pins, so the routes read from the
    // landing; then the part goes back so the command captures the real
    // "was". Each hop bumps so nothing reads a stale cache.
    p->rot = rot;
    p->mirror = mirror;
    s->bump();
    std::vector<WireRoute> routes = attached_wire_routes(Drag::Kind::Part, id);
    p->rot = was_rot;
    p->mirror = was_mirror;
    s->bump();
    push_transform_part_routed(*doc, id, rot, mirror, std::move(name),
                               std::move(routes));
    return true;
}

std::vector<std::size_t> Editor::annotation_order(const std::vector<Pos>& at,
                                                  double row_tol) {
    return ws::circsim::annotation_order(at, row_tol);
}

std::size_t Editor::annotate(double row_tol_cells) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr || doc == nullptr) {
        return 0;
    }
    struct Item {
        std::uint32_t id;
        Pos at;
        std::string ref;
    };
    std::map<std::string, std::vector<Item>> groups;
    std::size_t custom = 0;
    for (const ws::circsim::Part& p : s->parts()) {
        const ws::circsim::SymbolDef* sym = sym_of(p);
        if (sym == nullptr || sym->ref_prefix.empty()) {
            continue;  // no designator (grounds, flags...): not ours
        }
        if (!p.ref.empty() && !is_auto_ref(p.ref, sym->ref_prefix)) {
            ++custom;  // the author's name: kept
            continue;
        }
        groups[sym->ref_prefix].push_back(Item{p.id, p.pos, p.ref});
    }
    const double tol = row_tol_cells * grid_spec().base;
    std::vector<std::pair<std::uint32_t, std::string>> changes;
    std::size_t total = 0;
    std::size_t in_directives = 0;
    for (auto& entry : groups) {
        const std::string& prefix = entry.first;
        const std::vector<Item>& items = entry.second;
        std::vector<Pos> at;
        at.reserve(items.size());
        for (const Item& it : items) {
            at.push_back(it.at);
        }
        const std::vector<std::size_t> order = annotation_order(at, tol);
        for (std::size_t n = 0; n < order.size(); ++n) {
            const Item& it = items[order[n]];
            ++total;
            std::string want = prefix + std::to_string(n + 1);
            if (want != it.ref) {
                if (mentions_token(s->directives, it.ref)) {
                    ++in_directives;
                }
                changes.emplace_back(it.id, std::move(want));
            }
        }
    }
    if (total == 0u) {
        status("nothing to annotate - no parts with reference prefixes");
        return 0;
    }
    const std::size_t changed = push_annotate(*doc, std::move(changes));
    if (changed == 0u) {
        status("references already in reading order");
        return 0;
    }
    std::string msg = "annotated " + std::to_string(changed) +
                      (changed == 1u ? " reference" : " references");
    if (custom != 0u) {
        msg += " (" + std::to_string(custom) + " custom kept)";
    }
    if (in_directives != 0u) {
        // Renumbering can silently re-aim a directive that names a part
        // (.meas I(R3), .param...): say so, loudly, once.
        toast(msg + " - " + std::to_string(in_directives) +
                  " renamed ref(s) appear in your directives: check them",
              gui::ToastSeverity::warning, 6.0f);
    } else {
        status(msg);
    }
    return changed;
}

// The attached ortho wires' fresh shapes for a move of `id` (kind =
// the entity kind being moved), read at the entity's LANDING: the
// editor computes the new routes; push_move_* captures each wire's
// CURRENT waypoints as the undo shape. Without the re-route a moved
// endpoint left the adjacent wire legs diagonal (waypoints are
// absolute, not relative).
std::vector<WireRoute> Editor::attached_wire_routes(
    Drag::Kind kind, std::uint32_t id) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return {};
    }
    return ws::circsim::attached_wire_routes(
        *s, library,
        kind == Drag::Kind::Part ? ws::circsim::AttachedKind::Part
                                 : ws::circsim::AttachedKind::Junction,
        id);
}

std::optional<PinHit> Editor::hit_pin(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    return ws::circsim::hit_pin(*s, library, world, radius);
}

std::optional<std::uint32_t> Editor::hit_junction(Pos world,
                                                  double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    return ws::circsim::hit_junction(*s, world, radius);
}

// A point on an existing wire's path the cursor would land on (stage
// 18b): for every segment, project the cursor onto the segment, snap
// the projection to the fine grid, and accept the snap when it stays
// on the segment (the exact check nets.cpp will run later); otherwise
// fall back to the segment's nearest endpoint (a path vertex, always
// grid-aligned). Among all segments' candidates, return the one
// closest to the cursor. route_wire's paths are ortho-on-grid, so the
// common case lands inside the segment; the fallback is the rare
// near-endpoint click, where the vertex is the right answer anyway.
std::optional<Editor::WirePointSnap> Editor::hit_wire_point(
    Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    // The fine lattice the T-tap snaps to: the SAME spec the grid
    // renderer draws (the drawn dots and the snap targets can never
    // disagree), passed as a spacing - the lib owns no ui include.
    return ws::circsim::hit_wire_point(*s, library, world, radius,
                                       grid_spec().base);
}

Selection Editor::hit_any(Pos world, double radius) const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return {};
    }
    // Priority walks the DRAW STACK topmost-first: a junction dot
    // sits on every wire end and crossing; a wire runs over the
    // canvas and stays selectable even where it crosses or ends
    // inside a part's box (the box is bigger than the part's ink);
    // a click on a part body with no wire under it selects the part;
    // labels ride under everything. A click that lands on a pin
    // therefore still selects whatever wire lands there first - the
    // pin itself has no separate selection (the part's Properties
    // open from the part body).
    if (const std::uint32_t j = hit_junction(world, radius).value_or(0)) {
        return {Selection::Kind::Junction, j};
    }
    // The NEAREST wire in range - the loop once returned on the first
    // segment that matched, leaving its best_d tracking dead.
    std::uint32_t best_wire = 0;
    bool wire_found = false;
    double best_d = radius;
    for (const ws::circsim::Wire& w : s->wires()) {
        const std::vector<Pos>& path = wire_geom(w).path;
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double d = dist_to_segment(path[i - 1], path[i], world);
            if (d <= best_d) {
                best_d = d;
                best_wire = w.id;
                wire_found = true;
            }
        }
    }
    if (wire_found) {
        return {Selection::Kind::Wire, best_wire};
    }
    // The LAST part whose box holds the point: the parts draw in list
    // order, so the last one is the one on top (the first one used to
    // win - a stacked pair selected the buried one).
    std::uint32_t best_part = 0;
    bool part_found = false;
    for (const ws::circsim::Part& p : s->parts()) {
        const std::optional<Bounds> b = part_bounds(p.id);
        if (b.has_value() && world.x >= b->min_x && world.x <= b->max_x &&
            world.y >= b->min_y && world.y <= b->max_y) {
            best_part = p.id;  // no break: the topmost wins
            part_found = true;
        }
    }
    if (part_found) {
        return {Selection::Kind::Part, best_part};
    }
    // The label reach rides the zoom (a few screen px beside the pick
    // radius) - a fixed world-unit margin once answered clicks a full
    // grid cell (400px at max zoom) away from the text.
    const double label_reach =
        radius + (view.zoom > 0.0 ? kLabelPickPx / view.zoom : 0.0);
    for (const ws::circsim::Label& l : s->labels()) {
        if (len(Pos{l.pos.x - world.x, l.pos.y - world.y}) <= label_reach) {
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
    return ws::circsim::content_bounds(*s, library);
}

std::optional<Bounds> Editor::selection_bounds() const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return std::nullopt;
    }
    // The WORKING set's extent (the anchor alone was the old answer):
    // F frames what the group commands act on, box and all.
    BoundsBuilder out;
    for (const Selection& m : selection_set()) {
        switch (m.kind) {
            case Selection::Kind::None:
                break;
            case Selection::Kind::Part:
                if (const std::optional<Bounds> b = part_bounds(m.id)) {
                    out.add(*b);
                }
                break;
            case Selection::Kind::Junction:
                if (const ws::circsim::Junction* j =
                        s->find_junction(m.id)) {
                    out.add(j->pos);
                }
                break;
            case Selection::Kind::Wire:
                for (const ws::circsim::Wire& w : s->wires()) {
                    if (w.id == m.id) {
                        for (const Pos& p : wire_path(w)) {
                            out.add(p);
                        }
                        break;
                    }
                }
                break;
            case Selection::Kind::Label:
                for (const ws::circsim::Label& l : s->labels()) {
                    if (l.id == m.id) {
                        out.add(l.pos);
                        break;
                    }
                }
                break;
        }
    }
    return out.box;
}

// --- the working set -----------------------------------------------------------

std::vector<Selection> Editor::selection_set() const {
    const ws::circsim::Schematic* s = schem();
    std::vector<Selection> out;
    // The anchor first, then the multi entries - deduped by
    // (kind, id), and every id re-validated against the schematic so
    // an undo that landed under the selection drops cleanly.
    const auto push = [&](const Selection& m) {
        for (const Selection& have : out) {
            if (have.kind == m.kind && have.id == m.id) {
                return;
            }
        }
        bool live = false;
        if (s != nullptr) {
            switch (m.kind) {
                case Selection::Kind::Part:
                    live = s->find_part(m.id) != nullptr;
                    break;
                case Selection::Kind::Junction:
                    live = s->find_junction(m.id) != nullptr;
                    break;
                case Selection::Kind::Wire:
                    live = s->find_wire(m.id) != nullptr;
                    break;
                case Selection::Kind::Label:
                    live = s->find_label(m.id) != nullptr;
                    break;
                case Selection::Kind::None:
                    break;
            }
        }
        if (live) {
            out.push_back(m);
        }
    };
    if (sel.kind != Selection::Kind::None) {
        push(sel);
    }
    for (const Selection& m : multi) {
        push(m);
    }
    return out;
}

bool Editor::in_selection(const Selection& of) const {
    if (of.kind == Selection::Kind::None) {
        return false;
    }
    if (sel.kind == of.kind && sel.id == of.id) {
        return true;
    }
    for (const Selection& m : multi) {
        if (m.kind == of.kind && m.id == of.id) {
            return true;
        }
    }
    return false;
}

void Editor::set_selection(const Selection& of) {
    sel = of;
    multi.clear();
}

void Editor::toggle_selection(const Selection& of) {
    if (of.kind == Selection::Kind::None) {
        return;
    }
    for (std::size_t i = 0; i < multi.size(); ++i) {
        if (multi[i].kind == of.kind && multi[i].id == of.id) {
            // Toggle out: the anchor follows it out when it WAS the
            // anchor (the set loses its Properties subject honestly).
            const bool was_anchor = sel.kind == of.kind && sel.id == of.id;
            multi.erase(multi.begin() +
                        static_cast<std::ptrdiff_t>(i));
            if (was_anchor) {
                sel = multi.empty() ? Selection{} : multi.front();
            }
            return;
        }
    }
    multi.push_back(of);
    sel = of;  // the anchor follows the join (Properties shows it)
}

void Editor::select_box(const BoxSelect& b, bool extend) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    const auto [lo, hi] = b.rect();
    std::vector<Selection> found;
    const auto in_box = [&](const Bounds& bb) {
        return bb.max_x >= lo.x && bb.min_x <= hi.x && bb.max_y >= lo.y &&
               bb.min_y <= hi.y;
    };
    // The schematic's own order within each kind - the anchor is the
    // first part (else the first wire, junction, label) the box holds.
    for (const ws::circsim::Part& p : s->parts()) {
        if (const std::optional<Bounds> bb = part_bounds(p.id);
            bb.has_value() && in_box(*bb)) {
            found.push_back({Selection::Kind::Part, p.id});
        }
    }
    for (const ws::circsim::Wire& w : s->wires()) {
        for (const Pos& p : wire_path(w)) {
            if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y) {
                found.push_back({Selection::Kind::Wire, w.id});
                break;
            }
        }
    }
    for (const ws::circsim::Junction& j : s->junctions()) {
        if (j.pos.x >= lo.x && j.pos.x <= hi.x && j.pos.y >= lo.y &&
            j.pos.y <= hi.y) {
            found.push_back({Selection::Kind::Junction, j.id});
        }
    }
    for (const ws::circsim::Label& l : s->labels()) {
        if (l.pos.x >= lo.x && l.pos.x <= hi.x && l.pos.y >= lo.y &&
            l.pos.y <= hi.y) {
            found.push_back({Selection::Kind::Label, l.id});
        }
    }

    if (extend) {
        // Union with the current set (the toggles' own rule); the
        // anchor keeps its place unless it was nothing.
        for (const Selection& m : found) {
            bool have = in_selection(m);
            if (!have) {
                multi.push_back(m);
            }
        }
        if (sel.kind == Selection::Kind::None && !multi.empty()) {
            sel = multi.front();
        }
        return;
    }
    multi.clear();
    for (const Selection& m : found) {
        multi.push_back(m);
    }
    sel = multi.empty() ? Selection{} : multi.front();
}

// --- copy / paste / duplicate ---------------------------------------------------

bool Editor::copy_selection() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return false;
    }
    const std::vector<Selection> set = selection_set();
    if (set.empty()) {
        return false;
    }

    Clipboard clip;
    // The clipboard's own id space: a part keeps its ORIGINAL id (the
    // paste's part_map keys on it), while every junction the clipboard
    // carries - selected or boundary-minted - is renumbered to its
    // clipboard INDEX, and the carried wires' endpoints are rewritten
    // into that space at copy time. No clipboard JunctionRef can then
    // name an id the clipboard does not hold, and no two can collide.
    std::map<std::uint32_t, std::uint32_t> part_map;      // original -> clip
    std::map<std::uint32_t, std::uint32_t> junction_map;  // original -> index
    BoundsBuilder extent;

    // Pass 1: the parts, junctions and labels the set names.
    for (const Selection& m : set) {
        switch (m.kind) {
            case Selection::Kind::Part:
                if (const ws::circsim::Part* p = s->find_part(m.id)) {
                    part_map.emplace(p->id, p->id);
                    clip.parts.push_back(*p);
                }
                break;
            case Selection::Kind::Junction: {
                const ws::circsim::Junction* j = s->find_junction(m.id);
                if (j == nullptr) {
                    break;
                }
                const auto index = static_cast<std::uint32_t>(
                    clip.junctions.size());
                junction_map.emplace(j->id, index);
                ws::circsim::Junction copy = *j;
                copy.id = index;  // the clipboard's own numbering
                clip.junctions.push_back(copy);
                break;
            }
            case Selection::Kind::Label:
                if (const ws::circsim::Label* l = s->find_label(m.id)) {
                    clip.labels.push_back(*l);
                }
                break;
            case Selection::Kind::Wire:
            case Selection::Kind::None:
                break;
        }
    }

    // Pass 2: the wires, endpoints rewritten into the clipboard's id
    // space. A boundary endpoint (a pin of an unselected part, a
    // junction outside the set) is carried as a junction minted at
    // the endpoint's world position - the drawn stub survives the
    // copy verbatim, and the paste lands it as a real junction.
    for (const Selection& m : set) {
        if (m.kind != Selection::Kind::Wire) {
            continue;
        }
        const ws::circsim::Wire* w = s->find_wire(m.id);
        if (w == nullptr) {
            continue;
        }
        const std::vector<Pos> path = wire_path(*w);
        if (path.empty()) {
            continue;  // a dangling ref pair - nothing drawable to keep
        }
        ws::circsim::Wire copy = *w;
        const auto carry = [&](ws::circsim::WireEndpoint& end,
                               const Pos& at) {
            if (const ws::circsim::PinRef* pin =
                    std::get_if<ws::circsim::PinRef>(&end)) {
                if (part_map.count(pin->part) != 0u) {
                    return;  // the part rides along; paste remaps the ref
                }
            } else if (const ws::circsim::JunctionRef* j =
                           std::get_if<ws::circsim::JunctionRef>(&end)) {
                const auto it = junction_map.find(j->junction);
                if (it != junction_map.end()) {
                    end = ws::circsim::JunctionRef{it->second};
                    return;
                }
            }
            ws::circsim::Junction mint;
            mint.id = static_cast<std::uint32_t>(clip.junctions.size());
            mint.pos = at;
            clip.junctions.push_back(mint);
            end = ws::circsim::JunctionRef{mint.id};
        };
        carry(copy.a, path.front());
        carry(copy.b, path.back());
        clip.wires.push_back(std::move(copy));
        for (const Pos& p : path) {
            extent.add(p);
        }
    }

    // The center: everything the copy carries, so the paste's anchor
    // is where the user's eye was.
    for (const ws::circsim::Part& p : clip.parts) {
        if (const std::optional<Bounds> b = part_bounds(p.id)) {
            extent.add(*b);
        }
    }
    for (const ws::circsim::Junction& j : clip.junctions) {
        extent.add(j.pos);
    }
    for (const ws::circsim::Label& l : clip.labels) {
        extent.add(l.pos);
    }
    if (extent.box.has_value()) {
        clip.center = Pos{(extent.box->min_x + extent.box->max_x) * 0.5,
                          (extent.box->min_y + extent.box->max_y) * 0.5};
    }
    if (clip.empty()) {
        return false;
    }
    clipboard = std::move(clip);
    return true;
}

std::vector<Selection> Editor::paste_clipboard(Pos at) {
    ws::circsim::Schematic* s = schem();
    if (s == nullptr || clipboard.empty() || doc == nullptr) {
        return {};
    }

    // The lattice-aligned delta: the clipboard's center rides `at`,
    // and both ends snap so every pasted position lands ON the grid
    // (the copy's own positions are lattice points - placement and
    // the wire tool snap everything they mint).
    const Pos anchor = clipboard.center.value_or(Pos{0.0, 0.0});
    const Pos delta = snap(at) - snap(anchor);

    editor::PastePayload payload;
    std::map<std::uint32_t, std::uint32_t> part_fresh;   // original -> new
    std::map<std::uint32_t, std::uint32_t> junction_fresh;  // index -> new
    std::vector<std::uint32_t> wire_fresh;
    std::vector<std::uint32_t> label_fresh;
    // The refs this paste has already assigned: next_ref sees only
    // the live schematic, so two R-parts in one clipboard would each
    // answer the same next free number without the local tally.
    std::map<std::string, unsigned long> ref_taken;

    for (const ws::circsim::Part& p : clipboard.parts) {
        ws::circsim::Part next = p;
        next.id = s->alloc_id();
        part_fresh.emplace(p.id, next.id);
        next.pos = Pos{p.pos.x + delta.x, p.pos.y + delta.y};
        if (const ws::circsim::SymbolDef* sym = sym_of(p);
            sym != nullptr && !sym->ref_prefix.empty()) {
            std::string ref = next_ref(*s, *sym);
            unsigned long n =
                std::strtoul(ref.c_str() + sym->ref_prefix.size(), nullptr,
                             10);
            const auto taken = ref_taken.find(sym->ref_prefix);
            if (taken != ref_taken.end() && n <= taken->second) {
                n = taken->second + 1;  // this batch already took it
                ref = sym->ref_prefix + std::to_string(n);
            }
            ref_taken[sym->ref_prefix] = n;
            next.ref = ref;
        }
        payload.parts.push_back(std::move(next));
    }
    for (std::size_t i = 0; i < clipboard.junctions.size(); ++i) {
        const ws::circsim::Junction& j = clipboard.junctions[i];
        ws::circsim::Junction next = j;
        next.id = s->alloc_id();
        junction_fresh.emplace(static_cast<std::uint32_t>(i), next.id);
        next.pos = Pos{j.pos.x + delta.x, j.pos.y + delta.y};
        payload.junctions.push_back(std::move(next));
    }
    for (const ws::circsim::Wire& w : clipboard.wires) {
        ws::circsim::Wire next = w;
        next.id = s->alloc_id();
        wire_fresh.push_back(next.id);
        const auto remap = [&](ws::circsim::WireEndpoint& end) {
            if (ws::circsim::PinRef* pin =
                    std::get_if<ws::circsim::PinRef>(&end)) {
                const auto it = part_fresh.find(pin->part);
                if (it != part_fresh.end()) {
                    pin->part = it->second;
                }
            } else if (ws::circsim::JunctionRef* j =
                           std::get_if<ws::circsim::JunctionRef>(&end)) {
                const auto it = junction_fresh.find(j->junction);
                if (it != junction_fresh.end()) {
                    j->junction = it->second;
                }
            }
        };
        remap(next.a);
        remap(next.b);
        next.waypoints.reserve(w.waypoints.size());
        for (const Pos& p : w.waypoints) {
            next.waypoints.push_back(Pos{p.x + delta.x, p.y + delta.y});
        }
        payload.wires.push_back(std::move(next));
    }
    for (const ws::circsim::Label& l : clipboard.labels) {
        ws::circsim::Label next = l;
        next.id = s->alloc_id();
        label_fresh.push_back(next.id);
        next.pos = Pos{l.pos.x + delta.x, l.pos.y + delta.y};
        payload.labels.push_back(std::move(next));
    }

    push_paste(*doc, std::move(payload));

    // The pasted entities become the working selection (the anchor:
    // the first pasted part).
    std::vector<Selection> pasted;
    for (const auto& [_, id] : part_fresh) {
        pasted.push_back({Selection::Kind::Part, id});
    }
    for (const std::uint32_t id : wire_fresh) {
        pasted.push_back({Selection::Kind::Wire, id});
    }
    for (const auto& [_, id] : junction_fresh) {
        pasted.push_back({Selection::Kind::Junction, id});
    }
    for (const std::uint32_t id : label_fresh) {
        pasted.push_back({Selection::Kind::Label, id});
    }
    multi = pasted;
    sel = pasted.empty() ? Selection{} : pasted.front();
    return pasted;
}

std::vector<Selection> Editor::duplicate_selection() {
    if (!copy_selection()) {
        return {};
    }
    // The standard EDA nudge: one column over, one row down the
    // screen (the world's y is up, so -2) - close enough to see, far
    // enough to grab.
    const Pos at = clipboard.center.value_or(Pos{0.0, 0.0}) +
                   Pos{2.0, -2.0};
    return paste_clipboard(at);
}

bool Editor::zoom_to_selection(double margin) {
    const std::optional<Bounds> b = selection_bounds();
    if (!b.has_value()) {
        status("nothing selected to frame - click a part or wire first");
        return false;
    }
    const Pos mid{(b->min_x + b->max_x) * 0.5, (b->min_y + b->max_y) * 0.5};
    if (b->max_x - b->min_x <= 0.0 && b->max_y - b->min_y <= 0.0) {
        // A lone point: zooming "to fit" it would dive to max zoom.
        view.fly_to(mid, view.zoom);
    } else {
        view.fit(*b, margin);
    }
    pulse_at(mid);
    return true;
}

Pos Editor::place_snap(const ws::circsim::SymbolDef& sym, Pos world, int rot,
                       bool mirror, bool coarse, double magnet_radius) const {
    const Pos base = snap(world, coarse);
    if (magnet_radius <= 0.0) {
        return base;
    }
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return base;
    }
    // The pin magnetism is the lib's (ws::circsim::magnet_shift): the
    // ghost nudged whole so its nearest pin lands exactly on a real
    // pin within the radius - a part dropped near another one mates
    // pin-to-pin instead of a cell off.
    return ws::circsim::magnet_shift(*s, library, sym, base, rot, mirror,
                                     magnet_radius);
}

std::string_view Editor::hint() const {
    if (doc == nullptr) {
        return "Open or create a schematic to get started";
    }
    switch (tool) {
        case Tool::Select:
            if (drag.kind != Drag::Kind::None && drag.moved) {
                return "Release to drop - hold Shift for the coarse grid";
            }
            if (group_drag.active && group_drag.moved) {
                return "Release to drop the set - hold Shift for the "
                       "coarse grid";
            }
            if (box.active) {
                return "Release to select what the box holds - Shift "
                       "extends the set instead of replacing it";
            }
            if (!multi.empty()) {
                return "Drag moves the set - Ctrl+C/D copy/duplicate - "
                       "Del deletes - click empty space to clear";
            }
            if (sel.kind != Selection::Kind::None) {
                return "Drag to move - F frames - T tidies wires - "
                       "Shift+click adds to the set - drag empty space "
                       "to box-select";
            }
            return "Click to select - drag empty space to box-select - "
                   "wheel zooms - middle-drag or Space+drag pans - "
                   "Shift+A numbers the refs";
        case Tool::Place:
            return "Click to place - R rotates - M mirrors - Shift snaps "
                   "coarse - Esc or right-click to finish";
        case Tool::Wire:
            if (wire_draft.active) {
                return "Click a pin or junction to connect, or free space "
                       "for a new junction - / flips the corner";
            }
            return "Click a pin, junction or wire to start a run";
        case Tool::Label:
            return "Click to drop the label - it stays armed for the next";
        case Tool::Probe:
            return "Click a wire, pin or label to toggle its v(net) "
                   "curve - a part body toggles its i(element) current";
    }
    return {};
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
        pulse_at(Pos{(b->min_x + b->max_x) * 0.5, (b->min_y + b->max_y) * 0.5});
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
                              std::string header, std::uint64_t version) {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return;
    }
    problems.has = true;
    problems.from_run = from_run;
    problems.version = version;
    problems.header = std::move(header);
    problems.diags = std::move(diags);
    problems.lines.clear();
    // The cursor indexed the list this call just replaced; it resets
    // with it, so the walk starts over at the new check's first
    // finding instead of landing wherever the old one stood.
    problems_cursor = -1;
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
    // Synchronous check: the version it ran against IS the live one.
    refresh_problems(false, ws::circsim::run_erc(*s, build, &library),
                     "erc", s->version());
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

const std::vector<std::pair<Pos, std::uint32_t>>&
Editor::auto_junction_dots() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        join_dots_.clear();
        joins_valid_ = false;
        return join_dots_;
    }
    if (!joins_valid_ || joins_version_ != s->version()) {
        // The same seam ERC and the export read: one build_nets per
        // edit (millisecond work at editor scales), never per frame.
        // The NetList itself lands in net_cache_; the dots resolve
        // their nets through it AFTER the move (net_of_pin answers
        // from the indexed lookup maps).
        const ws::circsim::NetBuild build =
            ws::circsim::build_nets(*s, &library);
        nets_cache_ = std::move(build.nets);
        nets_version_ = s->version();
        nets_valid_ = true;
        join_dots_.clear();
        for (const ws::circsim::SilentJoin& j : build.silent_joins) {
            const ws::circsim::Net* n = nullptr;
            if (j.kind == ws::circsim::SilentJoin::Kind::PinOnWire) {
                n = nets_cache_.net_of_pin(ws::circsim::PinRef{j.part, j.pin});
            } else {
                n = nets_cache_.net_of_wire(j.other);
            }
            join_dots_.push_back(
                {j.pos, n != nullptr ? n->id : 0u});
        }
        joins_version_ = s->version();
        joins_valid_ = true;
    }
    return join_dots_;
}

const ws::circsim::NetList& Editor::net_cache() {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        nets_cache_ = ws::circsim::NetList{};
        nets_valid_ = false;
        return nets_cache_;
    }
    if (!nets_valid_ || nets_version_ != s->version()) {
        // auto_junction_dots shares this cache and refreshes it on its
        // own staleness; when the dots have not run this edit, the
        // coloring is the one that pays the build.
        const ws::circsim::NetBuild build =
            ws::circsim::build_nets(*s, &library);
        nets_cache_ = std::move(build.nets);
        nets_version_ = s->version();
        nets_valid_ = true;
        // The dots rode the OLD build; their cache must not outlive it.
        joins_valid_ = false;
    }
    return nets_cache_;
}

// The deck's node token for a net id (the probe's translator): the
// export map is token -> net, so the inversion walks it once - the
// first token a net answers wins, the deck's own one-node-per-net
// spelling (the overlay snapshot's own inversion).
[[nodiscard]] std::string net_token_of(
    const std::map<std::string, std::uint32_t, ws::circsim::CiLess>&
        node_net,
    std::uint32_t net_id) {
    for (const auto& [tok, id] : node_net) {
        if (id == net_id) {
            return tok;
        }
    }
    return {};
}

// --- the probe tool -----------------------------------------------------------

std::string Editor::probe_at(Pos world, double radius) {
    pulse_at(world);
    if (!results.has || results.run.results.tables.empty()) {
        return "probe: no results yet - run first (Ctrl+Enter)";
    }

    // What the click names: a pin (the smallest target) answers its
    // NET's voltage; then the entity stack - a wire, a junction or a
    // label answers a net, a part's BODY answers its element's
    // current (the pin/body split is the teachable rule: nets are
    // voltage, elements are current).
    std::string signal;
    if (const std::optional<PinHit> pin = hit_pin(world, radius)) {
        const ws::circsim::NetList& nets = net_cache();
        if (const ws::circsim::Net* n = nets.net_of_pin(pin->ref)) {
            signal =
                "v(" + net_token_of(results.map.node_net, n->id) + ")";
        }
    } else {
        const Selection hit = hit_any(world, radius);
        const ws::circsim::NetList& nets = net_cache();
        switch (hit.kind) {
            case Selection::Kind::Wire:
                if (const ws::circsim::Net* n =
                        nets.net_of_wire(hit.id)) {
                    signal = "v(" +
                             net_token_of(results.map.node_net, n->id) +
                             ")";
                }
                break;
            case Selection::Kind::Junction:
                if (const ws::circsim::Net* n =
                        nets.net_of_junction(hit.id)) {
                    signal = "v(" +
                             net_token_of(results.map.node_net, n->id) +
                             ")";
                }
                break;
            case Selection::Kind::Label: {
                const ws::circsim::Schematic* s = schem();
                const ws::circsim::Label* l =
                    s != nullptr ? s->find_label(hit.id) : nullptr;
                if (l != nullptr) {
                    if (const ws::circsim::Net* n = nets.by_name(l->name)) {
                        signal = "v(" +
                                 net_token_of(results.map.node_net,
                                              n->id) +
                                 ")";
                    }
                }
                break;
            }
            case Selection::Kind::Part: {
                // The element's current: the export back-map names the
                // deck element this part became (power/ground parts
                // emit no card - the honest "nothing to probe").
                for (const auto& [name, part_id] : results.map.element_part) {
                    if (part_id == hit.id) {
                        signal = "i(" + name + ")";
                        break;
                    }
                }
                break;
            }
            case Selection::Kind::None:
                break;
        }
    }
    if (signal.empty()) {
        return "probe: nothing probeable there - click a wire, a pin "
               "or a part body";
    }
    if (signal == "v(0)") {
        return "probe: ground is the reference - every v() is "
               "measured against it";
    }

    // The table that carries the signal: the canvas sweep table
    // first (the clock the strip and the plot share), else the first
    // table with the column. The panel switches to it, the flag
    // flips, and the legend's checkbox state follows.
    const std::vector<ws::spice::Table>& tables =
        results.run.results.tables;
    std::size_t table = results.table;
    if (const ws::spice::Table* sweep = canvas_sweep_table();
        sweep != nullptr) {
        const auto idx = sweep->signal_index(signal);
        if (idx.has_value()) {
            table = static_cast<std::size_t>(
                sweep - &tables.front());
        }
    }
    if (!(table < tables.size()) ||
        !tables[table].signal_index(signal).has_value()) {
        for (std::size_t i = 0; i < tables.size(); ++i) {
            if (tables[i].signal_index(signal).has_value()) {
                table = i;
                break;
            }
        }
    }
    if (table >= tables.size() ||
        !tables[table].signal_index(signal).has_value()) {
        return "probe: " + signal + " is not in the run";
    }
    results.table = table;
    const ws::spice::Table& t = tables[table];
    const auto index = t.signal_index(signal);
    if (results.visible_for != table ||
        results.visible.size() != t.signals.size()) {
        results.visible.assign(t.signals.size(), 1u);
        results.visible_for = table;
    }
    const bool shown = results.visible[*index] == 0u;
    results.visible[*index] = shown ? 1u : 0u;
    return std::string{"probe: "} + signal +
           (shown ? " shown" : " hidden");
}

const ws::spice::Table* Editor::canvas_sweep_table() const {
    if (!results.has || results.run.results.tables.empty()) {
        return nullptr;
    }
    const std::vector<ws::spice::Table>& tables =
        results.run.results.tables;
    // What the Results panel shows, when the transport scrubs it - a
    // .tran time axis or a .dc swept source - the canvas animates the
    // plot the user is looking at; a frequency table never drives the
    // clock (its x is a log grid, and the transport would lie).
    if (results.table < tables.size() &&
        ws::circsim::overlay_scrubbable(tables[results.table])) {
        return &tables[results.table];
    }
    for (const ws::spice::Table& t : tables) {
        if (ws::circsim::overlay_scrubbable(t)) {
            return &t;
        }
    }
    return nullptr;
}

std::size_t Editor::shown_sample(const ws::spice::Table& t,
                                 double playhead) {
    // The rule lives in ws::circsim (overlay.hpp) so the plot's hover
    // math, the canvas's paint and this seam share one answer.
    return ws::circsim::overlay_playhead_sample(t, playhead);
}

void Editor::document_changed() {
    tool = Tool::Select;
    place_symbol.clear();
    ghost_rot = 0;
    ghost_mirror = false;
    sel.clear();
    multi.clear();
    box = BoxSelect{};
    drag = Drag{};
    group_drag = GroupDrag{};
    wire_draft = WireDraft{};
    problems = ProblemsCache{};
    problems_cursor = -1;  // the rows it indexed are another document's
    netlist = NetlistCache{};
    results = ResultsCache{};
    playback = Playback{};      // another document has no transient yet
    transport_scrub = false;
    results_scrub = false;      // the row's drag dies with the document
    join_dots_.clear();
    joins_valid_ = false;
    nets_cache_ = ws::circsim::NetList{};
    nets_valid_ = false;
    part_bounds_cache_.clear();
    bounds_valid_ = false;
    wire_geom_cache_.clear();
    wire_geom_valid_ = false;
    // The Properties panel's field state rides too: a new document
    // whose first selection happens to share a kind and id with the
    // old one must never show the old document's typing.
    fields.clear();
    field_stamp = 1;
    fields_sel_kind = -1;
    fields_sel_id = 0;
    analysis_index = -1;
    const ws::circsim::Schematic* s = schem();
    view.cancel_glide();  // a document switch snaps, never glides
    if (s != nullptr) {
        // A saved zoom is file data: clamp it into the editor's range
        // (a hand-edited 0.001 or 1e9 must not brick the canvas).
        view.zoom = EditorView::clamp_zoom(
            s->view.zoom > 0.0 ? s->view.zoom : EditorView::kDefaultZoom);
        view.center = s->view.center;
    } else {
        view.zoom = EditorView::kDefaultZoom;
        view.center = {0.0, 0.0};
    }
    // The annotation state map is EDITOR lifetime (survives document
    // switches like wire_colors). It is intentionally NOT cleared here.
    // The annot_state_of accessor lazy-creates entries, so a fresh
    // document starts with empty state per part.
}

bool Editor::tick() {
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (tick_started_) {
        dt = std::chrono::duration<double>(now - last_tick_).count();
    }
    last_tick_ = now;
    tick_started_ = true;
    dt = std::clamp(dt, 0.0, 0.1);  // a stalled frame must not teleport

    bool busy = view.step(dt);
    if (pulse.active) {
        pulse.age += dt;
        if (pulse.age >= Pulse::kLife) {
            pulse.active = false;
        } else {
            busy = true;
        }
    }
    // The .tran/.dc playback advances with the same clock: the whole
    // run sweeps by in kLoopSeconds of wall time at 1x (the rate
    // stretches or shrinks it), looping at the window's end - the
    // step math is ws::circsim's own (overlay.hpp), headless and
    // tested, and the playhead's units are the sweep table's own
    // axis (seconds for a .tran, the swept source's value for a
    // .dc). A run with nothing scrubbable (or an empty table) parks
    // itself - the transport strip still scrubs by hand.
    if (playback.playing) {
        const ws::spice::Table* t = canvas_sweep_table();
        const std::span<const double> axis =
            t != nullptr ? ws::circsim::overlay_sweep_axis(*t)
                         : std::span<const double>{};
        if (axis.empty()) {
            playback.playing = false;
        } else {
            playback.t = ws::circsim::overlay_step(
                playback.t, ws::circsim::overlay_window(*t), dt,
                playback.rate, Playback::kLoopSeconds);
            busy = true;  // keep the frames coming
        }
    }
    return busy;
}

void Editor::toast(std::string text, gui::ToastSeverity severity,
                   float seconds) const {
    if (toasts != nullptr) {
        // Quiet mode: the same words while the last copy is still on
        // screen add nothing but noise.
        const auto now = std::chrono::steady_clock::now();
        if (dedupe_toasts && text == last_toast_ && now < last_toast_until_) {
            return;
        }
        last_toast_ = text;
        last_toast_until_ =
            now + std::chrono::duration_cast<
                      std::chrono::steady_clock::duration>(
                      std::chrono::duration<float>(seconds));
        // Toasts borrows the bytes, so the text gets its own stable
        // slot (toast_text_, see editor.hpp). The old function-static
        // holder was ONE buffer shared by every toast: two toasts in a
        // frame showed the same text, and the reassignment could free
        // the bytes an earlier, still-live toast pointed at.
        constexpr std::size_t kMaxLive = 16;
        toast_text_.push_back(std::move(text));
        while (toast_text_.size() > kMaxLive) {
            toast_text_.pop_front();
        }
        toasts->push({toast_text_.back().c_str(), severity, nullptr,
                      seconds});
    }
}

void Editor::status(std::string text) const {
    if (set_status) {
        set_status(std::move(text));
    }
}

// --- C1-C4: schematic readability pass (annotation placement engine) ---

bool Editor::annot_blocked(const ScreenRect& tile,
                           const AnnotObstacles& obs,
                           const EditorView& editor_view) const {
    // Project each obstacle to screen and test overlap with the tile.
    // A small margin (2 px) around the tile keeps text from kissing
    // wires and part bodies.
    const float margin = 2.0f;
    ScreenRect tile_pad = tile;
    tile_pad.x -= margin;
    tile_pad.y -= margin;
    tile_pad.w += 2.0f * margin;
    tile_pad.h += 2.0f * margin;

    // Part body AABBs
    for (const Bounds& b : obs.part_bodies) {
        ws::math::Vec2 min = editor_view.w2s({b.min_x, b.min_y});
        ws::math::Vec2 max = editor_view.w2s({b.max_x, b.max_y});
        ScreenRect r{std::min(min.x, max.x), std::min(min.y, max.y),
                     std::fabs(max.x - min.x), std::fabs(max.y - min.y)};
        if (r.w > 0.0f && r.h > 0.0f &&
            tile_pad.x < r.x + r.w && tile_pad.x + tile_pad.w > r.x &&
            tile_pad.y < r.y + r.h && tile_pad.y + tile_pad.h > r.y) {
            return true;
        }
    }

    // Wire polylines: project each segment to a thickened screen rect
    // (stroke width ~2 px in screen space). The view's zoom gives the
    // world-to-screen scale.
    const float stroke_px = 3.0f;  // world-to-screen will be applied
    for (const auto& path : obs.wire_paths) {
        for (std::size_t i = 1; i < path.size(); ++i) {
            ws::math::Vec2 a = editor_view.w2s(path[i - 1]);
            ws::math::Vec2 b = editor_view.w2s(path[i]);
            float min_x = std::min(a.x, b.x) - stroke_px;
            float max_x = std::max(a.x, b.x) + stroke_px;
            float min_y = std::min(a.y, b.y) - stroke_px;
            float max_y = std::max(a.y, b.y) + stroke_px;
            ScreenRect r{min_x, min_y, max_x - min_x, max_y - min_y};
            if (r.w > 0.0f && r.h > 0.0f &&
                tile_pad.x < r.x + r.w && tile_pad.x + tile_pad.w > r.x &&
                tile_pad.y < r.y + r.h && tile_pad.y + tile_pad.h > r.y) {
                return true;
            }
        }
    }

    // Junction dots (small circles, ~4 px radius in screen)
    const float dot_r = 5.0f;
    for (const Pos& j : obs.junctions) {
        ws::math::Vec2 p = editor_view.w2s(j);
        ScreenRect r{p.x - dot_r, p.y - dot_r, 2.0f * dot_r, 2.0f * dot_r};
        if (tile_pad.x < r.x + r.w && tile_pad.x + tile_pad.w > r.x &&
            tile_pad.y < r.y + r.h && tile_pad.y + tile_pad.h > r.y) {
            return true;
        }
    }

    // View bounds: if the tile would fall outside the view + margin, it's blocked.
    ws::math::Vec2 vmin = editor_view.w2s({obs.view_bounds.min_x, obs.view_bounds.min_y});
    ws::math::Vec2 vmax = editor_view.w2s({obs.view_bounds.max_x, obs.view_bounds.max_y});
    ScreenRect view_r{std::min(vmin.x, vmax.x) - margin,
                      std::min(vmin.y, vmax.y) - margin,
                      std::fabs(vmax.x - vmin.x) + 2.0f * margin,
                      std::fabs(vmax.y - vmin.y) + 2.0f * margin};
    if (tile_pad.x < view_r.x || tile_pad.y < view_r.y ||
        tile_pad.x + tile_pad.w > view_r.x + view_r.w ||
        tile_pad.y + tile_pad.h > view_r.y + view_r.h) {
        return true;
    }

    return false;
}

namespace {

[[nodiscard]] bool screen_rects_overlap(const ScreenRect& a,
                                        const ScreenRect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h &&
           a.y + a.h > b.y;
}

// Field-by-field, so no assumption about Bounds' member order.
[[nodiscard]] Bounds make_bounds(double x0, double y0, double x1, double y1) {
    Bounds b{};
    b.min_x = std::min(x0, x1);
    b.min_y = std::min(y0, y1);
    b.max_x = std::max(x0, x1);
    b.max_y = std::max(y0, y1);
    return b;
}

struct TilePick {
    ScreenRect rect;
    Pos anchor;
};

// Picks where a w x h pixel tile goes around `ideal`: the ideal itself
// when nothing blocks it, else the nearest free spot on a pixel-radius
// ring (below first, then around - radii in pixels so the search
// behaves the same at every zoom). When every candidate is blocked the
// tile stays at the ideal rather than wandering off. The old code
// scored the ideal candidate 0 WITHOUT asking whether it was blocked,
// so it always won and the whole ring search below it was dead code -
// labels sat on top of their own part bodies and wires.
[[nodiscard]] TilePick pick_tile(
    const EditorView& view, Pos ideal, float w, float h,
    const std::function<bool(const ScreenRect&)>& blocked) {
    const double zoom = view.zoom > 0.0 ? static_cast<double>(view.zoom) : 1.0;
    const auto rect_at = [&](Pos world) {
        const ws::math::Vec2 s = view.w2s(world);
        return ScreenRect{s.x - w * 0.5f, s.y - h * 0.5f, w, h};
    };
    const TilePick ideal_pick{rect_at(ideal), ideal};
    if (!blocked(ideal_pick.rect)) {
        return ideal_pick;
    }
    constexpr int kDirs = 8;
    constexpr double kRingPx[3] = {20.0, 44.0, 80.0};
    double best_score = 1e100;
    TilePick best = ideal_pick;
    for (const double r_px : kRingPx) {
        for (int d = 0; d < kDirs; ++d) {
            // Start straight below (y is up in world space) and walk
            // around: the first free spot at the smallest radius wins.
            const double ang = (-90.0 + d * 360.0 / kDirs) * kPi / 180.0;
            const Pos cand{ideal.x + r_px / zoom * std::cos(ang),
                           ideal.y + r_px / zoom * std::sin(ang)};
            const ScreenRect r = rect_at(cand);
            const double score = r_px + (blocked(r) ? 1000.0 : 0.0);
            if (score < best_score) {
                best_score = score;
                best = TilePick{r, cand};
            }
        }
    }
    return best_score >= 1000.0 ? ideal_pick : best;
}

}  // namespace

const Editor::AnnotPlacement& Editor::place_part_annotation() const {
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        annot_placement_cache_ = AnnotPlacement{};
        annot_placement_cache_valid_ = true;
        annot_placement_cache_signature_ = 0;
        return annot_placement_cache_;
    }

    // Hash every input used by placement. Annotation values are hashed
    // directly because UI code edits the returned state by reference.
    std::size_t signature = 0;
    const auto mix = [&signature](std::size_t value) {
        signature ^= value + static_cast<std::size_t>(0x9e3779b9u) +
                     (signature << 6u) + (signature >> 2u);
    };
    mix(std::hash<std::uint64_t>{}(s->version()));
    mix(std::hash<double>{}(view.zoom));
    mix(std::hash<double>{}(view.center.x));
    mix(std::hash<double>{}(view.center.y));
    mix(std::hash<float>{}(view.vx));
    mix(std::hash<float>{}(view.vy));
    mix(std::hash<float>{}(view.vw));
    mix(std::hash<float>{}(view.vh));
    mix(std::hash<const void*>{}(font));
    mix(std::hash<bool>{}(show_part_values));
    if (font != nullptr) {
        mix(std::hash<float>{}(font->line_height()));
    }
    for (const auto& [id, state] : annot_states_) {
        mix(std::hash<std::uint32_t>{}(id));
        mix(std::hash<double>{}(state.nudge.x));
        mix(std::hash<double>{}(state.nudge.y));
        mix(std::hash<bool>{}(state.annotate_off));
    }
    if (annot_placement_cache_valid_ &&
        annot_placement_cache_signature_ == signature) {
        return annot_placement_cache_;
    }

    AnnotPlacement result;

    // Build obstacles from the visible schematic.
    AnnotObstacles obs;
    // The VISIBLE world rect, not the content box: with the content box
    // every label that wanted to sit just outside the outermost parts
    // counted as out of bounds and was blocked.
    if (this->view.valid()) {
        const Pos wa = this->view.s2w(ws::math::Vec2{this->view.vx, this->view.vy});
        const Pos wb = this->view.s2w(ws::math::Vec2{this->view.vx + this->view.vw,
                                                     this->view.vy + this->view.vh});
        obs.view_bounds = make_bounds(wa.x, wa.y, wb.x, wb.y);
    } else {
        obs.view_bounds = content_bounds().value_or(Bounds{0, 0, 0, 0});
    }
    std::vector<ScreenRect> placed;  // tiles already laid down this frame

    // Part body AABBs
    for (const auto& part : s->parts()) {
        auto b = part_bounds(part.id);
        if (b) {
            obs.part_bodies.push_back(*b);
        }
    }

    // Wire polylines
    for (const auto& wire : s->wires()) {
        obs.wire_paths.push_back(wire_path(wire));
    }

    // Junction dots
    for (const auto& junc : s->junctions()) {
        obs.junctions.push_back(junc.pos);
    }

    // Part annotations
    if (show_part_values) {
        for (const auto& part : s->parts()) {
            // Skip if annotate_off
            const AnnotState& state = const_cast<Editor*>(this)->annot_state_of(part.id);
            if (state.annotate_off) {
                continue;
            }

            const ws::circsim::SymbolDef* sym = sym_of(part);
            if (sym == nullptr) {
                continue;
            }

            // Refdes
            std::string refdes = part.ref;
            if (refdes.empty()) {
                continue;  // no refdes, nothing to annotate
            }

            // Value/model line: the part's `value` param if present,
            // else the model param, else empty.
            std::string value;
            auto it_val = part.params.find("value");
            if (it_val != part.params.end() && !it_val->second.empty()) {
                value = it_val->second;
            } else {
                auto it_mod = part.params.find("model");
                if (it_mod != part.params.end() && !it_mod->second.empty()) {
                    value = it_mod->second;
                }
            }

            // Text origin from symbol (if any) - not available in current API,
            // fall back to part bounds center-bottom
            Pos text_origin{0.0, 0.0};
            bool has_origin = false;

            PartAnnotation ann;
            ann.part_id = part.id;
            ann.refdes = std::move(refdes);
            ann.value = std::move(value);
            ann.text_origin = text_origin;
            ann.has_origin = has_origin;

            // Ideal anchor: text_origin if has_origin, else part bounds center-bottom
            Pos ideal_anchor;
            if (has_origin) {
                ideal_anchor = text_origin;
            } else {
                auto b = part_bounds(part.id);
                if (b) {
                    ideal_anchor = {b->min_x + (b->max_x - b->min_x) * 0.5, b->min_y};
                } else {
                    ideal_anchor = part.pos;
                }
            }

            // Measure with the real face when there is one (the old
            // 8 px/char guess clipped or over-padded every tile).
            const bool have_font =
                font != nullptr && font->line_height() > 0.0f;
            const std::string line_text =
                ann.value.empty() ? ann.refdes : ann.refdes + " " + ann.value;
            const float text_w =
                have_font ? font->measure_text(line_text.c_str())
                          : static_cast<float>(line_text.size()) * 8.0f;
            const float line_h = have_font ? font->line_height() : 14.0f;
            const double zoom_px =
                this->view.zoom > 0.0 ? static_cast<double>(this->view.zoom) : 1.0;
            if (!has_origin) {
                // Below the body, not centred on its edge: the tile's
                // top clears the bottom edge by a few pixels.
                ideal_anchor.y -= (line_h * 0.5 + 3.0) / zoom_px;
            }

            const TilePick pick = pick_tile(
                this->view, ideal_anchor, text_w, line_h,
                [&](const ScreenRect& r) {
                    if (annot_blocked(r, obs, this->view)) {
                        return true;
                    }
                    for (const ScreenRect& other : placed) {
                        if (screen_rects_overlap(r, other)) {
                            return true;
                        }
                    }
                    return false;
                });

            // Apply the per-part nudge on top of the engine's answer.
            const Pos final_anchor = {pick.anchor.x + state.nudge.x,
                                      pick.anchor.y + state.nudge.y};
            const ws::math::Vec2 final_screen = this->view.w2s(final_anchor);
            const ScreenRect final_rect{final_screen.x - text_w * 0.5f,
                                        final_screen.y - line_h * 0.5f,
                                        text_w, line_h};
            placed.push_back(final_rect);
            ws::ui::gui::Rect gui_rect{final_rect.x, final_rect.y,
                                       final_rect.w, final_rect.h};
            result.part_tiles.push_back(
                {ann.refdes, ann.value, gui_rect, final_anchor});
        }
    }

    // Net labels
    // The placed part tiles are obstacles for the net names (a name
    // drawn across a refdes reads as one garbled word).
    AnnotObstacles net_obs = obs;
    for (const TextTile& t : result.part_tiles) {
        const Pos a = this->view.s2w(ws::math::Vec2{t.screen_rect.x, t.screen_rect.y});
        const Pos b = this->view.s2w(
            ws::math::Vec2{t.screen_rect.x + t.screen_rect.width,
                           t.screen_rect.y + t.screen_rect.height});
        net_obs.part_bodies.push_back(make_bounds(a.x, a.y, b.x, b.y));
    }
    result.net_tiles = place_net_label(net_obs, this->view);
    result.obstacles = std::move(obs);
    annot_placement_cache_ = std::move(result);
    annot_placement_cache_signature_ = signature;
    annot_placement_cache_valid_ = true;
    return annot_placement_cache_;
}

std::vector<Editor::TextTile> Editor::place_net_label(
    const AnnotObstacles& obs, const EditorView& editor_view) const {
    std::vector<TextTile> tiles;
    const ws::circsim::Schematic* s = schem();
    if (s == nullptr) {
        return tiles;
    }

    // Reuse the editor's versioned net cache; rebuilding the same netlist
    // here every frame duplicated work already done by wire coloring.
    const ws::circsim::NetList& nets = const_cast<Editor*>(this)->net_cache();
    if (nets.nets.empty()) {
        return tiles;
    }

    std::vector<ScreenRect> placed;  // names already laid down

    for (const auto& net : nets.nets) {
        // Skip ground net (name "0" or "gnd" or "GND")
        std::string name = net.name;
        if (name == "0" || name == "gnd" || name == "GND") {
            continue;
        }

        // Compute centroid of all pins/junctions on this net
        double cx = 0.0, cy = 0.0;
        int count = 0;
        for (const auto& pin : net.pins) {
            const ws::circsim::Part* part = s->find_part(pin.part);
            if (part != nullptr) {
                const ws::circsim::SymbolDef* sym = sym_of(*part);
                if (sym != nullptr) {
                    // Pin position in world coords using pin_pos
                    ws::circsim::Pos pin_pos = ws::circsim::pin_pos(*sym, *part, pin.pin);
                    cx += pin_pos.x;
                    cy += pin_pos.y;
                    ++count;
                }
            }
        }
        for (std::uint32_t jid : net.junctions) {
            const ws::circsim::Junction* j = s->find_junction(jid);
            if (j != nullptr) {
                cx += j->pos.x;
                cy += j->pos.y;
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        Pos centroid{cx / count, cy / count};

        const bool have_font = font != nullptr && font->line_height() > 0.0f;
        const float text_w = have_font
                                 ? font->measure_text(name.c_str())
                                 : static_cast<float>(name.size()) * 8.0f;
        const float line_h = have_font ? font->line_height() : 14.0f;

        const TilePick pick = pick_tile(
            editor_view, centroid, text_w, line_h,
            [&](const ScreenRect& r) {
                if (annot_blocked(r, obs, editor_view)) {
                    return true;
                }
                for (const ScreenRect& other : placed) {
                    if (screen_rects_overlap(r, other)) {
                        return true;
                    }
                }
                return false;
            });
        placed.push_back(pick.rect);
        const ws::ui::gui::Rect gui_rect{pick.rect.x, pick.rect.y,
                                         pick.rect.w, pick.rect.h};
        tiles.push_back({name, "", gui_rect, pick.anchor});
    }

    return tiles;
}

Editor::AnnotState& Editor::annot_state_of(std::uint32_t part_id) {
    return annot_states_[part_id];
}

void Editor::set_annot_state(std::uint32_t part_id, const AnnotState& st) {
    annot_states_[part_id] = st;
    annot_placement_cache_valid_ = false;
}

void draw_symbol(canvas::Canvas2D& c, const ws::circsim::SymbolDef& sym,
                 Pos pos, int rot, bool mirror, const Paint& body,
                 std::uint32_t text_color, const ws::ui::r2d::Font* font,
                 double switch_closed) {
    // The geometry lives in ws::circsim's render seam (render_symbol):
    // one pure flattening shared with the CLI's SVG dump and the tests.
    // This is the painting half only - strokes to canvas calls.
    ws::circsim::Part placement{};
    placement.pos = pos;
    placement.rot = rot;
    placement.mirror = mirror;
    constexpr double kRad = kPi / 180.0;
    for (const ws::circsim::SymbolStroke& s :
         ws::circsim::render_symbol(sym, placement, switch_closed)) {
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
