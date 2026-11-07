// apps/circsim_editor - the document layer (see document.hpp). The
// command set is deliberately uniform: every command is a value
// snapshot pair (or an entity list) plus the two writers, and the
// merge_with hooks coalesce only same-kind typing runs - the scratch
// pad's DocEdit precedent, applied per field.

#include "document.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <memory>
#include <set>
#include <utility>

namespace editor {
namespace {

using ws::circsim::Junction;
using ws::circsim::JunctionRef;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Schematic;
using ws::circsim::Wire;
using ws::circsim::WireEndpoint;

// --- the container writers (the loader's paths: ids stay, bump after)

template <typename T, typename IdOf>
T take_entity(std::deque<T>& container, std::uint32_t id, IdOf&& id_of) {
    const auto at = std::find_if(container.begin(), container.end(),
                                 [&](const T& t) { return id_of(t) == id; });
    if (at == container.end()) {
        return T{};  // callers validated; a no-op keeps undo sound
    }
    T out = *at;
    container.erase(at);
    return out;
}

template <typename T>
void put_back(std::deque<T>& container, T entity) {
    container.push_back(std::move(entity));
}

Part take_part(Schematic& s, std::uint32_t id) {
    return take_entity(s.parts_mut(), id,
                       [](const Part& p) { return p.id; });
}
Wire take_wire(Schematic& s, std::uint32_t id) {
    return take_entity(s.wires_mut(), id,
                       [](const Wire& w) { return w.id; });
}
Junction take_junction(Schematic& s, std::uint32_t id) {
    return take_entity(s.junctions_mut(), id,
                       [](const Junction& j) { return j.id; });
}
ws::circsim::Label take_label(Schematic& s, std::uint32_t id) {
    return take_entity(s.labels_mut(), id, [](const ws::circsim::Label& l) {
        return l.id;
    });
}

void reinsert_part(Schematic& s, Part p) {
    s.raise_next_id(p.id);
    put_back(s.parts_mut(), std::move(p));
    s.bump();
}
void reinsert_wire(Schematic& s, Wire w) {
    s.raise_next_id(w.id);
    put_back(s.wires_mut(), std::move(w));
    s.bump();
}
void reinsert_junction(Schematic& s, Junction j) {
    s.raise_next_id(j.id);
    put_back(s.junctions_mut(), std::move(j));
    s.bump();
}
void reinsert_label(Schematic& s, ws::circsim::Label l) {
    s.raise_next_id(l.id);
    put_back(s.labels_mut(), std::move(l));
    s.bump();
}

// The junctions `removing` orphans: referenced by one of its wires,
// by no wire outside the set. Pure - the callers build payloads
// without mutating, and the command's apply() owns the one true
// removal. The "outside the set" test is keyed by id, which holds
// because every wire owns a distinct one (placement mints, the
// loader preserves disk ids) - the removed-id set states that
// contract up front instead of per-wire "the one being removed"
// guesses (the identity shortcut would NOT work here: `removing`
// holds copies, never the schematic's own elements).
[[nodiscard]] std::vector<Junction> orphaned_junctions(
    const Schematic& s, const std::vector<Wire>& removing) {
    std::set<std::uint32_t> removed;
    for (const Wire& w : removing) {
        removed.insert(w.id);
    }
    std::vector<Junction> out;
    for (const Wire& w : removing) {
        for (const WireEndpoint end : {w.a, w.b}) {
            const JunctionRef* j = std::get_if<JunctionRef>(&end);
            if (j == nullptr) {
                continue;
            }
            bool other = false;
            for (const Wire& w2 : s.wires()) {
                if (removed.count(w2.id) != 0u) {
                    continue;  // one of the ones being removed
                }
                const JunctionRef* a = std::get_if<JunctionRef>(&w2.a);
                const JunctionRef* b = std::get_if<JunctionRef>(&w2.b);
                if ((a != nullptr && a->junction == j->junction) ||
                    (b != nullptr && b->junction == j->junction)) {
                    other = true;
                    break;
                }
            }
            if (!other) {
                if (const Junction* found = s.find_junction(j->junction)) {
                    out.push_back(*found);
                }
            }
        }
    }
    return out;
}

// --- the command base: every command names the schematic it edits ---

class EditCommand : public ws::app::UndoCommand {
  public:
    explicit EditCommand(SchematicDocument* doc) : doc_(doc) {}

  protected:
    Schematic& schem() { return doc_->schem; }

  private:
    SchematicDocument* doc_;
};

}  // namespace

// --- the document ---------------------------------------------------------

SchematicDocument::SchematicDocument(std::string path,
                                     ws::settings::FileSystem& fs)
    : path_(std::move(path)), fs_(&fs) {}

const std::string& SchematicDocument::path() const { return path_; }
void SchematicDocument::set_path(std::string path) {
    path_ = std::move(path);
}

std::string SchematicDocument::display_name() const {
    if (path_.empty()) {
        return "untitled";
    }
    const std::size_t slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? path_ : path_.substr(slash + 1);
}

std::string SchematicDocument::doc_type() const { return "csim"; }

ws::core::Status SchematicDocument::load() {
    report = ws::circsim::LoadReport{};
    const auto loaded = ws::circsim::read_csim(*fs_, path_, report);
    if (!loaded.has_value()) {
        return ws::core::unexpected<ws::core::Error>(loaded.error());
    }
    schem = std::move(*loaded);
    (void)undo_.clear();
    (void)undo_.mark_saved();
    return {};
}

ws::core::Status SchematicDocument::save() {
    return ws::circsim::write_csim(*fs_, path_, schem);
}

ws::app::UndoStack& SchematicDocument::undo() { return undo_; }
std::uint64_t SchematicDocument::job_group() const { return 0; }

std::string SchematicDocument::autosave_content() const {
    return ws::circsim::save_csim(schem);
}

void SchematicDocument::adopt_autosave_content(std::string_view bytes) {
    ws::circsim::LoadReport adopted;
    const auto recovered = ws::circsim::load_csim(bytes, adopted);
    if (recovered.has_value()) {
        schem = *recovered;
        report = adopted;
    }
    // A crashed draft is unsaved work: clear() leaves the save point
    // unreachable, which IS the dirty flag (the stack's own rule).
    (void)undo_.clear();
}

// --- parts ----------------------------------------------------------------

namespace {

class AddPartCommand final : public EditCommand {
  public:
    AddPartCommand(SchematicDocument* doc, Part part)
        : EditCommand(doc), part_(std::move(part)) {}

    void apply() override { reinsert_part(schem(), part_); }
    void undo() override {
        (void)take_part(schem(), part_.id);
        schem().bump();
    }
    [[nodiscard]] std::string name() const override { return "Place Part"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + part_.symbol.size() + part_.ref.size() +
               part_.value.size();
    }

  private:
    Part part_;
};

class DeletePartCommand final : public EditCommand {
  public:
    DeletePartCommand(SchematicDocument* doc, Part part,
                      std::vector<Wire> wires, std::vector<Junction> junctions)
        : EditCommand(doc),
          part_(std::move(part)),
          wires_(std::move(wires)),
          junctions_(std::move(junctions)) {}

    void apply() override {
        // Wires first (they name the part), then the part, then the
        // junctions the removal orphaned.
        for (const Wire& w : wires_) {
            (void)take_wire(schem(), w.id);
        }
        (void)take_part(schem(), part_.id);
        for (const Junction& j : junctions_) {
            (void)take_junction(schem(), j.id);
        }
        schem().bump();
    }
    void undo() override {
        reinsert_part(schem(), part_);
        for (const Junction& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        for (const Wire& w : wires_) {
            reinsert_wire(schem(), w);
        }
    }
    [[nodiscard]] std::string name() const override { return "Delete Part"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + wires_.size() * sizeof(Wire) +
               junctions_.size() * sizeof(Junction);
    }

  private:
    Part part_;
    std::vector<Wire> wires_;
    std::vector<Junction> junctions_;
};

class MovePartCommand final : public EditCommand {
  public:
    MovePartCommand(SchematicDocument* doc, std::uint32_t id,
                    ws::circsim::Pos from, ws::circsim::Pos to,
                    std::vector<WireRoute> routes,
                    std::vector<WireRoute> undo_routes)
        : EditCommand(doc), id_(id), from_(from), to_(to),
          routes_(std::move(routes)), undo_routes_(std::move(undo_routes)) {}

    void apply() override {
        land(to_);
        land_routes(routes_);
    }
    void undo() override {
        land(from_);
        land_routes(undo_routes_);
    }
    [[nodiscard]] std::string name() const override { return "Move Part"; }

  private:
    // The attached wires' waypoints ride the same step as the part
    // (the endpoints resolve live through their refs; without the
    // re-route the adjacent legs would go diagonal). A wire the step
    // no longer finds is skipped - the undo shape stays honest.
    void land_routes(const std::vector<WireRoute>& routes) {
        for (const WireRoute& r : routes) {
            for (Wire& w : schem().wires_mut()) {
                if (w.id == r.wire) {
                    w.waypoints = r.waypoints;
                    break;
                }
            }
        }
    }
    void land(ws::circsim::Pos at) {
        if (Part* p = schem().find_part(id_)) {
            p->pos = at;
            schem().bump();
        }
    }
    std::uint32_t id_;
    ws::circsim::Pos from_, to_;
    std::vector<WireRoute> routes_, undo_routes_;
};

// Lands each route's waypoints on its wire (a wire the step no longer
// finds is skipped - the undo shape stays honest). True if any wire was
// touched. Shared by the commands that re-route alongside an edit
// (Transform Part, Tidy Wires).
bool land_wire_routes(ws::circsim::Schematic& s,
                      const std::vector<WireRoute>& routes) {
    bool any = false;
    for (const WireRoute& r : routes) {
        for (Wire& w : s.wires_mut()) {
            if (w.id == r.wire) {
                w.waypoints = r.waypoints;
                any = true;
                break;
            }
        }
    }
    return any;
}

[[nodiscard]] std::size_t routes_bytes(const std::vector<WireRoute>& routes) {
    std::size_t n = routes.size() * sizeof(WireRoute);
    for (const WireRoute& r : routes) {
        n += r.waypoints.size() * sizeof(ws::circsim::Pos);
    }
    return n;
}

// Rotate / mirror. A rotation moves the part's PINS, so the attached
// ortho wires ride the same undo step with fresh shapes (the move
// command's contract: without the re-route the adjacent legs go
// diagonal). The routes are optional - the plain seam passes none.
class TransformPartCommand final : public EditCommand {
  public:
    TransformPartCommand(SchematicDocument* doc, std::uint32_t id, int rot,
                         bool mirror, int was_rot, bool was_mirror,
                         std::string name, std::vector<WireRoute> routes = {},
                         std::vector<WireRoute> undo_routes = {})
        : EditCommand(doc),
          id_(id),
          rot_(rot),
          mirror_(mirror),
          was_rot_(was_rot),
          was_mirror_(was_mirror),
          name_(std::move(name)),
          routes_(std::move(routes)),
          undo_routes_(std::move(undo_routes)) {}

    void apply() override {
        land(rot_, mirror_);
        (void)land_wire_routes(schem(), routes_);
    }
    void undo() override {
        land(was_rot_, was_mirror_);
        (void)land_wire_routes(schem(), undo_routes_);
    }
    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + name_.size() + routes_bytes(routes_) +
               routes_bytes(undo_routes_);
    }

  private:
    void land(int rot, bool mirror) {
        if (Part* p = schem().find_part(id_)) {
            p->rot = rot;
            p->mirror = mirror;
            schem().bump();
        }
    }
    std::uint32_t id_;
    int rot_;
    bool mirror_;
    int was_rot_;
    bool was_mirror_;
    std::string name_;
    std::vector<WireRoute> routes_, undo_routes_;
};

// One part text field: ref (kind 0), value (kind 1), params[key] (2).
// The typing-run coalescer: same part, same kind, same key - the run's
// end wins, its start stands.
class PartTextCommand final : public EditCommand {
  public:
    PartTextCommand(SchematicDocument* doc, std::uint32_t id, int kind,
                    std::string param, std::string was, std::string next,
                    std::string name)
        : EditCommand(doc),
          id_(id),
          kind_(kind),
          param_(std::move(param)),
          was_(std::move(was)),
          next_(std::move(next)),
          name_(std::move(name)) {}

    void apply() override { land(next_); }
    void undo() override { land(was_); }
    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] bool merge_with(
        const ws::app::UndoCommand& newer) override {
        const auto* edit = dynamic_cast<const PartTextCommand*>(&newer);
        if (edit == nullptr || id_ != edit->id_ || kind_ != edit->kind_ ||
            param_ != edit->param_) {
            return false;
        }
        next_ = edit->next_;
        return true;
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + was_.size() + next_.size() + param_.size();
    }

  private:
    void land(const std::string& text) {
        Part* p = schem().find_part(id_);
        if (p == nullptr) {
            return;
        }
        switch (kind_) {
            case 0: p->ref = text; break;
            case 1: p->value = text; break;
            default:
                if (text.empty()) {
                    p->params.erase(param_);
                } else {
                    p->params[param_] = text;
                }
                break;
        }
        schem().bump();
    }

    std::uint32_t id_;
    int kind_;
    std::string param_;
    std::string was_, next_;
    std::string name_;
};

}  // namespace

void push_place_part(SchematicDocument& doc, ws::circsim::Part part) {
    // Ids start at 1 (0 is the "none" sentinel), so a zero id is a
    // caller that never minted one. The canvas mints at construction;
    // this seam mint guarantees no future placement path can land an
    // entity the find/take/select machinery would key wrong (the
    // id-0 wire class: every hand-drawn entity colliding on 0).
    if (part.id == 0) {
        part.id = doc.schem.alloc_id();
    }
    (void)doc.undo().push(
        std::make_unique<AddPartCommand>(&doc, std::move(part)));
}

void push_delete_part(SchematicDocument& doc, std::uint32_t part_id) {
    const Schematic& s = doc.schem;
    const Part* part = s.find_part(part_id);
    if (part == nullptr) {
        return;
    }
    std::vector<Wire> wires;
    for (const Wire& w : s.wires()) {
        const PinRef* a = std::get_if<PinRef>(&w.a);
        const PinRef* b = std::get_if<PinRef>(&w.b);
        if ((a != nullptr && a->part == part_id) ||
            (b != nullptr && b->part == part_id)) {
            wires.push_back(w);
        }
    }
    std::vector<Junction> junctions = orphaned_junctions(s, wires);
    (void)doc.undo().push(std::make_unique<DeletePartCommand>(
        &doc, *part, std::move(wires), std::move(junctions)));
}

void push_move_part(SchematicDocument& doc, std::uint32_t part_id,
                    ws::circsim::Pos to,
                    std::vector<WireRoute> wire_routes) {
    const Part* p = doc.schem.find_part(part_id);
    if (p == nullptr) {
        return;
    }
    const ws::circsim::Pos from = p->pos;
    // The undo shape: every routed wire's CURRENT waypoints (a wire
    // the routes mention but the schematic lost is just skipped).
    std::vector<WireRoute> undo_routes;
    undo_routes.reserve(wire_routes.size());
    for (const WireRoute& r : wire_routes) {
        if (const Wire* w = doc.schem.find_wire(r.wire)) {
            undo_routes.push_back(WireRoute{r.wire, w->waypoints});
        }
    }
    (void)doc.undo().push(std::make_unique<MovePartCommand>(
        &doc, part_id, from, to, std::move(wire_routes),
        std::move(undo_routes)));
}

void push_transform_part(SchematicDocument& doc, std::uint32_t part_id,
                         int rot, bool mirror, std::string name) {
    const Part* p = doc.schem.find_part(part_id);
    if (p == nullptr) {
        return;
    }
    (void)doc.undo().push(std::make_unique<TransformPartCommand>(
        &doc, part_id, rot, mirror, p->rot, p->mirror, std::move(name)));
}

void push_transform_part_routed(SchematicDocument& doc, std::uint32_t part_id,
                                int rot, bool mirror, std::string name,
                                std::vector<WireRoute> wire_routes) {
    const Part* p = doc.schem.find_part(part_id);
    if (p == nullptr) {
        return;
    }
    // The undo shape: every routed wire's CURRENT waypoints.
    std::vector<WireRoute> undo_routes;
    undo_routes.reserve(wire_routes.size());
    for (const WireRoute& r : wire_routes) {
        if (const Wire* w = doc.schem.find_wire(r.wire)) {
            undo_routes.push_back(WireRoute{r.wire, w->waypoints});
        }
    }
    (void)doc.undo().push(std::make_unique<TransformPartCommand>(
        &doc, part_id, rot, mirror, p->rot, p->mirror, std::move(name),
        std::move(wire_routes), std::move(undo_routes)));
}

void push_part_text(SchematicDocument& doc, std::uint32_t part_id, int kind,
                    std::string param, std::string next, std::string name) {
    const Part* p = doc.schem.find_part(part_id);
    if (p == nullptr) {
        return;
    }
    std::string was;
    switch (kind) {
        case 0: was = p->ref; break;
        case 1: was = p->value; break;
        default: {
            const auto it = p->params.find(param);
            was = it != p->params.end() ? it->second : "";
            break;
        }
    }
    if (was == next) {
        return;
    }
    (void)doc.undo().push(std::make_unique<PartTextCommand>(
        &doc, part_id, kind, std::move(param), std::move(was),
        std::move(next), std::move(name)));
}

// --- wires & junctions ----------------------------------------------------

namespace {

class AddWireCommand final : public EditCommand {
  public:
    AddWireCommand(SchematicDocument* doc, Wire wire,
                   std::vector<Junction> junctions)
        : EditCommand(doc), wire_(std::move(wire)),
          junctions_(std::move(junctions)) {}

    void apply() override {
        for (const Junction& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        reinsert_wire(schem(), wire_);
    }
    void undo() override {
        (void)take_wire(schem(), wire_.id);
        for (const Junction& j : junctions_) {
            (void)take_junction(schem(), j.id);
        }
        schem().bump();
    }
    [[nodiscard]] std::string name() const override { return "Place Wire"; }

  private:
    Wire wire_;
    std::vector<Junction> junctions_;
};

class DeleteWireCommand final : public EditCommand {
  public:
    DeleteWireCommand(SchematicDocument* doc, Wire wire,
                      std::vector<Junction> junctions)
        : EditCommand(doc), wire_(std::move(wire)),
          junctions_(std::move(junctions)) {}

    void apply() override {
        (void)take_wire(schem(), wire_.id);
        for (const Junction& j : junctions_) {
            (void)take_junction(schem(), j.id);
        }
        schem().bump();
    }
    void undo() override {
        for (const Junction& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        reinsert_wire(schem(), wire_);
    }
    [[nodiscard]] std::string name() const override { return "Delete Wire"; }

  private:
    Wire wire_;
    std::vector<Junction> junctions_;
};

class MoveJunctionCommand final : public EditCommand {
  public:
    MoveJunctionCommand(SchematicDocument& doc, std::uint32_t id,
                        ws::circsim::Pos from, ws::circsim::Pos to,
                        std::vector<WireRoute> routes,
                        std::vector<WireRoute> undo_routes)
        : EditCommand(&doc), id_(id), from_(from), to_(to),
          routes_(std::move(routes)), undo_routes_(std::move(undo_routes)) {}

    void apply() override {
        land(to_);
        land_routes(routes_);
    }
    void undo() override {
        land(from_);
        land_routes(undo_routes_);
    }
    [[nodiscard]] std::string name() const override {
        return "Move Junction";
    }

  private:
    // The attached wires ride the step, same as the part move's.
    void land_routes(const std::vector<WireRoute>& routes) {
        for (const WireRoute& r : routes) {
            for (Wire& w : schem().wires_mut()) {
                if (w.id == r.wire) {
                    w.waypoints = r.waypoints;
                    break;
                }
            }
        }
    }
    void land(ws::circsim::Pos at) {
        // Junctions are loader-managed ids: the *_mut() walk.
        for (Junction& j : schem().junctions_mut()) {
            if (j.id == id_) {
                j.pos = at;
                schem().bump();
                return;
            }
        }
    }
    std::uint32_t id_;
    ws::circsim::Pos from_, to_;
    std::vector<WireRoute> routes_, undo_routes_;
};

// "Tidy Wires": re-straightens a set of wires in ONE undo step. Pure
// waypoint swaps - no entity moves, no ids change - so apply() lands
// the new shapes and undo() lands the captured old ones. Wires the step
// no longer finds are skipped (the same honesty the move commands keep).
class TidyWiresCommand final : public EditCommand {
  public:
    TidyWiresCommand(SchematicDocument* doc, std::vector<WireRoute> routes,
                     std::vector<WireRoute> undo_routes)
        : EditCommand(doc), routes_(std::move(routes)),
          undo_routes_(std::move(undo_routes)) {}

    void apply() override { land(routes_); }
    void undo() override { land(undo_routes_); }
    [[nodiscard]] std::string name() const override { return "Tidy Wires"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + routes_bytes(routes_) +
               routes_bytes(undo_routes_);
    }

  private:
    void land(const std::vector<WireRoute>& routes) {
        if (land_wire_routes(schem(), routes)) {
            schem().bump();  // the move commands bump through land(pos)
        }
    }
    std::vector<WireRoute> routes_, undo_routes_;
};

// "Annotate": a whole-sheet renumbering as ONE undo step. `next` and
// `was` are (part id, ref) lists over the SAME parts, so the final refs
// land together - no transient duplicates, nothing to merge.
class AnnotateCommand final : public EditCommand {
  public:
    using Refs = std::vector<std::pair<std::uint32_t, std::string>>;
    AnnotateCommand(SchematicDocument* doc, Refs next, Refs was)
        : EditCommand(doc), next_(std::move(next)), was_(std::move(was)) {}

    void apply() override { land(next_); }
    void undo() override { land(was_); }
    [[nodiscard]] std::string name() const override { return "Annotate"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        std::size_t n = sizeof(*this);
        for (const auto& r : next_) { n += sizeof(r) + r.second.size(); }
        for (const auto& r : was_) { n += sizeof(r) + r.second.size(); }
        return n;
    }

  private:
    void land(const Refs& refs) {
        bool any = false;
        for (const auto& r : refs) {
            if (Part* p = schem().find_part(r.first)) {
                p->ref = r.second;
                any = true;
            }
        }
        if (any) {
            schem().bump();
        }
    }
    Refs next_, was_;
};

[[nodiscard]] bool same_waypoints(const std::vector<ws::circsim::Pos>& a,
                                  const std::vector<ws::circsim::Pos>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y) {
            return false;
        }
    }
    return true;
}

}  // namespace

void push_place_wire(SchematicDocument& doc, ws::circsim::Wire wire,
                     std::vector<ws::circsim::Junction> junctions) {
    // The belt-and-braces twin of push_place_part's mint: 0 is the
    // "none" sentinel, so a wire that arrives un-minted gets its id
    // here - the command layer preserves the id it is handed, and
    // every id-keyed consumer (find/take_wire, Selection::is, the
    // orphan scan) presupposes uniqueness.
    if (wire.id == 0) {
        wire.id = doc.schem.alloc_id();
    }
    (void)doc.undo().push(std::make_unique<AddWireCommand>(
        &doc, std::move(wire), std::move(junctions)));
}

void push_delete_wire(SchematicDocument& doc, std::uint32_t wire_id) {
    const Schematic& s = doc.schem;
    const Wire* found = s.find_wire(wire_id);
    if (found == nullptr) {
        return;
    }
    const std::vector<Wire> one{*found};
    (void)doc.undo().push(std::make_unique<DeleteWireCommand>(
        &doc, *found, orphaned_junctions(s, one)));
}

void push_move_junction(SchematicDocument& doc, std::uint32_t junction_id,
                        ws::circsim::Pos to,
                        std::vector<WireRoute> wire_routes) {
    for (const Junction& j : doc.schem.junctions()) {
        if (j.id == junction_id) {
            std::vector<WireRoute> undo_routes;
            undo_routes.reserve(wire_routes.size());
            for (const WireRoute& r : wire_routes) {
                if (const Wire* w = doc.schem.find_wire(r.wire)) {
                    undo_routes.push_back(WireRoute{r.wire, w->waypoints});
                }
            }
            (void)doc.undo().push(std::make_unique<MoveJunctionCommand>(
                doc, junction_id, j.pos, to, std::move(wire_routes),
                std::move(undo_routes)));
            return;
        }
    }
}

std::size_t push_tidy_wires(SchematicDocument& doc,
                            std::vector<WireRoute> routes) {
    // Keep only the routes that CHANGE something: an already-tidy wire
    // must not pad the undo step, and a tidy that changes nothing must
    // not land an empty undo entry at all.
    std::vector<WireRoute> apply;
    std::vector<WireRoute> undo;
    for (WireRoute& r : routes) {
        const Wire* w = doc.schem.find_wire(r.wire);
        if (w == nullptr || same_waypoints(w->waypoints, r.waypoints)) {
            continue;
        }
        undo.push_back(WireRoute{r.wire, w->waypoints});
        apply.push_back(std::move(r));
    }
    if (apply.empty()) {
        return 0;
    }
    const std::size_t changed = apply.size();
    (void)doc.undo().push(std::make_unique<TidyWiresCommand>(
        &doc, std::move(apply), std::move(undo)));
    return changed;
}

std::size_t push_annotate(
    SchematicDocument& doc,
    std::vector<std::pair<std::uint32_t, std::string>> changes) {
    // Only the parts whose ref actually changes ride the step; a no-op
    // renumbering lands nothing on the undo stack.
    AnnotateCommand::Refs next;
    AnnotateCommand::Refs was;
    for (auto& c : changes) {
        const Part* p = doc.schem.find_part(c.first);
        if (p == nullptr || p->ref == c.second) {
            continue;
        }
        was.emplace_back(c.first, p->ref);
        next.push_back(std::move(c));
    }
    if (next.empty()) {
        return 0;
    }
    const std::size_t changed = next.size();
    (void)doc.undo().push(std::make_unique<AnnotateCommand>(
        &doc, std::move(next), std::move(was)));
    return changed;
}

// --- labels ---------------------------------------------------------------

namespace {

class AddLabelCommand final : public EditCommand {
  public:
    AddLabelCommand(SchematicDocument* doc, ws::circsim::Label label)
        : EditCommand(doc), label_(std::move(label)) {}

    void apply() override { reinsert_label(schem(), label_); }
    void undo() override {
        (void)take_label(schem(), label_.id);
        schem().bump();
    }
    [[nodiscard]] std::string name() const override { return "Place Label"; }

  private:
    ws::circsim::Label label_;
};

class DeleteLabelCommand final : public EditCommand {
  public:
    DeleteLabelCommand(SchematicDocument* doc, ws::circsim::Label label)
        : EditCommand(doc), label_(std::move(label)) {}

    void apply() override {
        (void)take_label(schem(), label_.id);
        schem().bump();
    }
    void undo() override { reinsert_label(schem(), label_); }
    [[nodiscard]] std::string name() const override { return "Delete Label"; }

  private:
    ws::circsim::Label label_;
};

class MoveLabelCommand final : public EditCommand {
  public:
    MoveLabelCommand(SchematicDocument* doc, std::uint32_t id,
                     ws::circsim::Pos from, ws::circsim::Pos to)
        : EditCommand(doc), id_(id), from_(from), to_(to) {}

    void apply() override { land(to_); }
    void undo() override { land(from_); }
    [[nodiscard]] std::string name() const override { return "Move Label"; }

  private:
    void land(ws::circsim::Pos at) {
        for (ws::circsim::Label& l : schem().labels_mut()) {
            if (l.id == id_) {
                l.pos = at;
                schem().bump();
                return;
            }
        }
    }
    std::uint32_t id_;
    ws::circsim::Pos from_, to_;
};

class LabelTextCommand final : public EditCommand {
  public:
    LabelTextCommand(SchematicDocument* doc, std::uint32_t id,
                     std::string was, std::string next)
        : EditCommand(doc), id_(id), was_(std::move(was)),
          next_(std::move(next)) {}

    void apply() override { land(next_); }
    void undo() override { land(was_); }
    [[nodiscard]] std::string name() const override { return "Edit Label"; }
    [[nodiscard]] bool merge_with(
        const ws::app::UndoCommand& newer) override {
        const auto* edit = dynamic_cast<const LabelTextCommand*>(&newer);
        if (edit == nullptr || id_ != edit->id_) {
            return false;
        }
        next_ = edit->next_;
        return true;
    }

  private:
    void land(const std::string& text) {
        for (ws::circsim::Label& l : schem().labels_mut()) {
            if (l.id == id_) {
                l.name = text;
                schem().bump();
                return;
            }
        }
    }
    std::uint32_t id_;
    std::string was_, next_;
};

}  // namespace

void push_place_label(SchematicDocument& doc, ws::circsim::Label label) {
    // Same mint as the part and wire seams: 0 is the "none" sentinel.
    if (label.id == 0) {
        label.id = doc.schem.alloc_id();
    }
    (void)doc.undo().push(
        std::make_unique<AddLabelCommand>(&doc, std::move(label)));
}

void push_delete_label(SchematicDocument& doc, std::uint32_t label_id) {
    for (const ws::circsim::Label& l : doc.schem.labels()) {
        if (l.id == label_id) {
            (void)doc.undo().push(
                std::make_unique<DeleteLabelCommand>(&doc, l));
            return;
        }
    }
}

void push_move_label(SchematicDocument& doc, std::uint32_t label_id,
                     ws::circsim::Pos to) {
    for (const ws::circsim::Label& l : doc.schem.labels()) {
        if (l.id == label_id) {
            (void)doc.undo().push(
                std::make_unique<MoveLabelCommand>(&doc, label_id, l.pos, to));
            return;
        }
    }
}

void push_label_text(SchematicDocument& doc, std::uint32_t label_id,
                     std::string next) {
    for (const ws::circsim::Label& l : doc.schem.labels()) {
        if (l.id == label_id) {
            if (l.name != next) {
                (void)doc.undo().push(std::make_unique<LabelTextCommand>(
                    &doc, label_id, l.name, std::move(next)));
            }
            return;
        }
    }
}

// --- directives & analysis --------------------------------------------------

namespace {

class DirectivesCommand final : public EditCommand {
  public:
    DirectivesCommand(SchematicDocument* doc, std::string was,
                      std::string next)
        : EditCommand(doc), was_(std::move(was)), next_(std::move(next)) {}

    void apply() override { schem().directives = next_; }
    void undo() override { schem().directives = was_; }
    [[nodiscard]] std::string name() const override {
        return "Edit Directives";
    }
    [[nodiscard]] bool merge_with(
        const ws::app::UndoCommand& newer) override {
        const auto* edit = dynamic_cast<const DirectivesCommand*>(&newer);
        if (edit == nullptr) {
            return false;
        }
        next_ = edit->next_;
        return true;
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + was_.size() + next_.size();
    }

  private:
    std::string was_, next_;
};

class AddAnalysisCommand final : public EditCommand {
  public:
    AddAnalysisCommand(SchematicDocument& doc,
                       ws::circsim::AnalysisSpec spec)
        : EditCommand(&doc), spec_(spec) {}

    void apply() override {
        schem().analysis.entries.push_back(spec_);
        schem().bump();
    }
    void undo() override {
        auto& entries = schem().analysis.entries;
        if (!entries.empty()) {
            entries.pop_back();
            schem().bump();
        }
    }
    [[nodiscard]] std::string name() const override {
        return "Add Analysis";
    }

  private:
    ws::circsim::AnalysisSpec spec_;
};

class RemoveAnalysisCommand final : public EditCommand {
  public:
    RemoveAnalysisCommand(SchematicDocument& doc, std::size_t index,
                          ws::circsim::AnalysisSpec spec)
        : EditCommand(&doc), index_(index), spec_(spec) {}

    void apply() override {
        auto& entries = schem().analysis.entries;
        if (index_ < entries.size()) {
            entries.erase(entries.begin() +
                          static_cast<std::ptrdiff_t>(index_));
            schem().bump();
        }
    }
    void undo() override {
        auto& entries = schem().analysis.entries;
        if (index_ <= entries.size()) {
            entries.insert(entries.begin() +
                               static_cast<std::ptrdiff_t>(index_),
                           spec_);
            schem().bump();
        }
    }
    [[nodiscard]] std::string name() const override {
        return "Remove Analysis";
    }

  private:
    std::size_t index_;
    ws::circsim::AnalysisSpec spec_;
};

class SetAnalysisCommand final : public EditCommand {
  public:
    SetAnalysisCommand(SchematicDocument& doc, std::size_t index,
                       ws::circsim::AnalysisSpec was,
                       ws::circsim::AnalysisSpec next)
        : EditCommand(&doc), index_(index), was_(was), next_(next) {}

    void apply() override { land(next_); }
    void undo() override { land(was_); }
    [[nodiscard]] std::string name() const override {
        return "Edit Analysis";
    }

  private:
    void land(const ws::circsim::AnalysisSpec& spec) {
        auto& entries = schem().analysis.entries;
        if (index_ < entries.size()) {
            entries[index_] = spec;
            schem().bump();
        }
    }
    std::size_t index_;
    ws::circsim::AnalysisSpec was_, next_;
};

}  // namespace

void push_directives(SchematicDocument& doc, std::string next) {
    if (doc.schem.directives == next) {
        return;
    }
    (void)doc.undo().push(std::make_unique<DirectivesCommand>(
        &doc, doc.schem.directives, std::move(next)));
}

void push_add_analysis(SchematicDocument& doc,
                       ws::circsim::AnalysisSpec spec) {
    (void)doc.undo().push(
        std::make_unique<AddAnalysisCommand>(doc, spec));
}

void push_remove_analysis(SchematicDocument& doc, std::size_t index) {
    auto& entries = doc.schem.analysis.entries;
    if (index < entries.size()) {
        (void)doc.undo().push(std::make_unique<RemoveAnalysisCommand>(
            doc, index, entries[index]));
    }
}

void push_set_analysis(SchematicDocument& doc, std::size_t index,
                       ws::circsim::AnalysisSpec next) {
    auto& entries = doc.schem.analysis.entries;
    if (index < entries.size()) {
        (void)doc.undo().push(std::make_unique<SetAnalysisCommand>(
            doc, index, entries[index], next));
    }
}

// --- reference designators ---------------------------------------------------

std::string next_ref(const ws::circsim::Schematic& s,
                     const ws::circsim::SymbolDef& sym) {
    if (sym.ref_prefix.empty()) {
        return "";
    }
    unsigned long highest = 0;
    for (const Part& p : s.parts()) {
        if (p.ref.size() <= sym.ref_prefix.size()) {
            continue;
        }
        if (p.ref.compare(0, sym.ref_prefix.size(), sym.ref_prefix) != 0) {
            continue;
        }
        const std::string tail = p.ref.substr(sym.ref_prefix.size());
        bool digits = true;
        for (const char c : tail) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                digits = false;
                break;
            }
        }
        if (digits) {
            highest = std::max(highest, std::strtoul(tail.c_str(), nullptr, 10));
        }
    }
    return sym.ref_prefix + std::to_string(highest + 1);
}

}  // namespace editor
