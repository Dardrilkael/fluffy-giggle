// apps/circsim_editor - the document layer (see document.hpp). The
// command set is deliberately uniform: every command is a value
// snapshot pair (or an entity list) plus the two writers, and the
// merge_with hooks coalesce only same-kind typing runs - the scratch
// pad's DocEdit precedent, applied per field.

#include "document.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <set>
#include <unordered_set>
#include <utility>

namespace editor {
namespace {

[[nodiscard]] std::uint64_t next_document_serial() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;  // 0 = none
}

using ws::circsim::Junction;
using ws::circsim::JunctionRef;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Schematic;
using ws::circsim::Wire;
using ws::circsim::WireEndpoint;

// Deletions must remember where each entity lived. Re-inserting at the
// tail is not the inverse of erasing from the middle of a deque.
template <typename T>
struct IndexedEntity {
    std::size_t index = 0;
    T value{};
};

template <typename T, typename IdOf>
std::vector<IndexedEntity<T>> capture_positions(
    const std::deque<T>& container, const std::vector<T>& selected,
    IdOf&& id_of) {
    std::unordered_set<std::uint32_t> wanted;
    wanted.reserve(selected.size());
    for (const T& entity : selected) {
        wanted.insert(id_of(entity));
    }

    std::vector<IndexedEntity<T>> out;
    out.reserve(wanted.size());
    for (std::size_t i = 0; i < container.size(); ++i) {
        const T& entity = container[i];
        if (wanted.erase(id_of(entity)) != 0u) {
            out.push_back(IndexedEntity<T>{i, entity});
        }
    }
    return out;  // original container order, with duplicate ids collapsed
}

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

template <typename T>
void put_back_at(std::deque<T>& container,
                 const IndexedEntity<T>& snapshot) {
    const std::size_t index = std::min(snapshot.index, container.size());
    container.insert(container.begin() +
                         static_cast<std::ptrdiff_t>(index),
                     snapshot.value);
}

void reinsert_part(Schematic& s, const IndexedEntity<Part>& p) {
    s.raise_next_id(p.value.id);
    put_back_at(s.parts_mut(), p);
    s.bump();
}
void reinsert_wire(Schematic& s, const IndexedEntity<Wire>& w) {
    s.raise_next_id(w.value.id);
    put_back_at(s.wires_mut(), w);
    s.bump();
}
void reinsert_junction(Schematic& s, const IndexedEntity<Junction>& j) {
    s.raise_next_id(j.value.id);
    put_back_at(s.junctions_mut(), j);
    s.bump();
}
void reinsert_label(Schematic& s,
                    const IndexedEntity<ws::circsim::Label>& l) {
    s.raise_next_id(l.value.id);
    put_back_at(s.labels_mut(), l);
    s.bump();
}

// The junctions `removing` orphans: referenced by a removed wire,
// but by no surviving wire. The scan is linear in the wire and junction
// counts; sets de-duplicate both removed ids and shared endpoint junctions.
[[nodiscard]] std::vector<Junction> orphaned_junctions(
    const Schematic& s, const std::vector<Wire>& removing) {
    std::unordered_set<std::uint32_t> removed;
    std::unordered_set<std::uint32_t> candidates;
    std::unordered_set<std::uint32_t> surviving;

    removed.reserve(removing.size());
    candidates.reserve(removing.size() * 2u);
    surviving.reserve(s.wires().size() * 2u);

    const auto collect_junction = [](const WireEndpoint& endpoint,
                                     std::unordered_set<std::uint32_t>& ids) {
        if (const JunctionRef* ref = std::get_if<JunctionRef>(&endpoint)) {
            ids.insert(ref->junction);
        }
    };

    for (const Wire& w : removing) {
        removed.insert(w.id);
        collect_junction(w.a, candidates);
        collect_junction(w.b, candidates);
    }

    for (const Wire& w : s.wires()) {
        if (removed.count(w.id) != 0u) {
            continue;
        }
        collect_junction(w.a, surviving);
        collect_junction(w.b, surviving);
    }

    std::vector<Junction> out;
    out.reserve(candidates.size());
    for (const Junction& junction : s.junctions()) {
        if (candidates.count(junction.id) != 0u &&
            surviving.count(junction.id) == 0u) {
            out.push_back(junction);
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
    : path_(std::move(path)), fs_(&fs), serial_(next_document_serial()) {}

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
std::uint64_t SchematicDocument::job_group() const { return serial_; }

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
    DeletePartCommand(SchematicDocument* doc, IndexedEntity<Part> part,
                      std::vector<IndexedEntity<Wire>> wires,
                      std::vector<IndexedEntity<Junction>> junctions)
        : EditCommand(doc), part_(std::move(part)), wires_(std::move(wires)),
          junctions_(std::move(junctions)) {}

    void apply() override {
        // Wires first (they name the part), then the part, then the
        // junctions the removal orphaned.
        for (const auto& w : wires_) {
            (void)take_wire(schem(), w.value.id);
        }
        (void)take_part(schem(), part_.value.id);
        for (const auto& j : junctions_) {
            (void)take_junction(schem(), j.value.id);
        }
        schem().bump();
    }
    void undo() override {
        reinsert_part(schem(), part_);
        for (const auto& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        for (const auto& w : wires_) {
            reinsert_wire(schem(), w);
        }
    }
    [[nodiscard]] std::string name() const override { return "Delete Part"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + wires_.size() * sizeof(IndexedEntity<Wire>) +
               junctions_.size() * sizeof(IndexedEntity<Junction>);
    }

  private:
    IndexedEntity<Part> part_;
    std::vector<IndexedEntity<Wire>> wires_;
    std::vector<IndexedEntity<Junction>> junctions_;
};

class MovePartCommand final : public EditCommand {
  public:
    // glued: the junctions riding the part's pins, each with its own
    // from/to POSITION pair (the pusher resolves the pin landings with
    // the library in hand - the command owns no symbol knowledge).
    MovePartCommand(SchematicDocument* doc, std::uint32_t id,
                    ws::circsim::Pos from, ws::circsim::Pos to,
                    std::vector<WireRoute> routes,
                    std::vector<WireRoute> undo_routes,
                    std::vector<GluedJunctionMove> glued)
        : EditCommand(doc), id_(id), from_(from), to_(to),
          routes_(std::move(routes)), undo_routes_(std::move(undo_routes)),
          glued_(std::move(glued)) {}

    void apply() override {
        land(to_, true);
        land_routes(routes_);
    }
    void undo() override {
        land(from_, false);
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
    void land(ws::circsim::Pos at, bool forward) {
        if (Part* p = schem().find_part(id_)) {
            p->pos = at;
            schem().bump();
        }
        // The glued junctions ride the pin they sit on: a wire end's
        // junction, a T-tap dropped there - stranded at the old spot
        // they would break exactly the connection the author drew.
        // Apply lands each glue's `to` (the pin's landing), undo its
        // `from` (the pin's press position) - one undo step returns
        // the whole ride.
        for (const GluedJunctionMove& g : glued_) {
            for (Junction& j : schem().junctions_mut()) {
                if (j.id == g.junction) {
                    j.pos = forward ? g.to : g.from;
                    break;
                }
            }
        }
    }
    std::uint32_t id_;
    ws::circsim::Pos from_, to_;
    std::vector<WireRoute> routes_, undo_routes_;
    std::vector<GluedJunctionMove> glued_;
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
// The glued junctions ride the pins' landings the same way (the
// pusher resolves both orientations with the library in hand: apply's
// glue `to`, undo's glue `from` - a rotated pin lands somewhere new,
// not a delta of the old spot).
class TransformPartCommand final : public EditCommand {
  public:
    TransformPartCommand(SchematicDocument* doc, std::uint32_t id, int rot,
                         bool mirror, int was_rot, bool was_mirror,
                         std::string name, std::vector<WireRoute> routes = {},
                         std::vector<WireRoute> undo_routes = {},
                         std::vector<GluedJunctionMove> glue = {})
        : EditCommand(doc),
          id_(id),
          rot_(rot),
          mirror_(mirror),
          was_rot_(was_rot),
          was_mirror_(was_mirror),
          name_(std::move(name)),
          routes_(std::move(routes)),
          undo_routes_(std::move(undo_routes)),
          glue_(std::move(glue)) {}

    void apply() override {
        land(rot_, mirror_);
        (void)land_wire_routes(schem(), routes_);
        land_glue(true);
    }
    void undo() override {
        land(was_rot_, was_mirror_);
        (void)land_wire_routes(schem(), undo_routes_);
        land_glue(false);
    }
    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + name_.size() + routes_bytes(routes_) +
               routes_bytes(undo_routes_);
    }

  private:
    // The glued junctions ride their pins: apply lands the `to`s
    // (each a NEW-orientation pin spot), undo the `from`s.
    void land_glue(bool forward) {
        for (const GluedJunctionMove& g : glue_) {
            for (Junction& j : schem().junctions_mut()) {
                if (j.id == g.junction) {
                    j.pos = forward ? g.to : g.from;
                    break;
                }
            }
        }
    }
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
    std::vector<GluedJunctionMove> glue_;
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

// One source's DC/AC mode flip: the value row and the wave param are
// ONE stimulus, so the flip lands both halves in one step (push_
// source_mode is the only caller). No merge_with on purpose - every
// switch is its own named step, never glued to the next one.
class SourceModeCommand final : public EditCommand {
  public:
    SourceModeCommand(SchematicDocument* doc, std::uint32_t id,
                      std::string was_value, std::string next_value,
                      std::string was_wave, std::string next_wave)
        : EditCommand(doc),
          id_(id),
          was_value_(std::move(was_value)),
          next_value_(std::move(next_value)),
          was_wave_(std::move(was_wave)),
          next_wave_(std::move(next_wave)) {}

    void apply() override { land(next_value_, next_wave_); }
    void undo() override { land(was_value_, was_wave_); }
    [[nodiscard]] std::string name() const override {
        return "Set source mode";
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + was_value_.size() + next_value_.size() +
               was_wave_.size() + next_wave_.size();
    }

  private:
    // The pair lands exactly the way the two PartTextCommand halves
    // would: the value verbatim, an empty wave ERASED (a blank param
    // never rides the model).
    void land(const std::string& value, const std::string& wave) {
        Part* p = schem().find_part(id_);
        if (p == nullptr) {
            return;
        }
        p->value = value;
        if (wave.empty()) {
            p->params.erase("wave");
        } else {
            p->params["wave"] = wave;
        }
        schem().bump();
    }

    std::uint32_t id_;
    std::string was_value_, next_value_;
    std::string was_wave_, next_wave_;
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
    const auto indexed_parts = capture_positions(
        s.parts(), std::vector<Part>{*part},
        [](const Part& p) { return p.id; });
    const auto indexed_wires = capture_positions(
        s.wires(), wires, [](const Wire& w) { return w.id; });
    const auto indexed_junctions = capture_positions(
        s.junctions(), junctions, [](const Junction& j) { return j.id; });
    if (indexed_parts.empty()) {
        return;
    }
    (void)doc.undo().push(std::make_unique<DeletePartCommand>(
        &doc, indexed_parts.front(), indexed_wires, indexed_junctions));
}

void push_move_part(SchematicDocument& doc, std::uint32_t part_id,
                    ws::circsim::Pos to,
                    std::vector<WireRoute> wire_routes,
                    std::vector<GluedJunctionMove> glued) {
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
        std::move(undo_routes), std::move(glued)));
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
                                std::vector<WireRoute> wire_routes,
                                std::vector<GluedJunctionMove> glue) {
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
        std::move(wire_routes), std::move(undo_routes),
        std::move(glue)));
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

void push_source_mode(SchematicDocument& doc, std::uint32_t part_id,
                      std::string next_value, std::string next_wave) {
    const Part* p = doc.schem.find_part(part_id);
    if (p == nullptr) {
        return;
    }
    const auto it = p->params.find("wave");
    const std::string was_wave = it != p->params.end() ? it->second : "";
    if (p->value == next_value && was_wave == next_wave) {
        return;  // already there: no step, no bump
    }
    (void)doc.undo().push(std::make_unique<SourceModeCommand>(
        &doc, part_id, p->value, std::move(next_value), was_wave,
        std::move(next_wave)));
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
            // A junction that already exists is the re-bind case (the
            // author clicked an existing dot on a wire's body): take
            // the original out first so the bound snapshot REPLACES it
            // (put_back appends; a blind reinsert would double the
            // dot). The snapshot carries the dot's own taps plus the
            // new binding - nothing is lost.
            const bool existing =
                schem().find_junction(j.id) != nullptr;
            if (existing) {
                (void)take_junction(schem(), j.id);
            }
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

// The junctions a move/rotate landing minted: one per CREATED contact
// (a moved pin on another wire's interior), each at the contact
// position. apply mints them (the silent pickup the landing made
// becomes a named node with a drawn dot); undo takes them exactly
// back - the connection the move created and the connection the undo
// removes ride the same step.
class ContactJunctionsCommand final : public EditCommand {
  public:
    ContactJunctionsCommand(SchematicDocument* doc,
                             std::vector<Junction> junctions,
                             std::string name)
        : EditCommand(doc), junctions_(std::move(junctions)),
          name_(std::move(name)) {}

    void apply() override {
        for (const Junction& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        schem().bump();
    }
    void undo() override {
        for (const Junction& j : junctions_) {
            (void)take_junction(schem(), j.id);
        }
        schem().bump();
    }
    [[nodiscard]] std::string name() const override { return name_; }

  private:
    std::vector<Junction> junctions_;
    std::string name_;
};

class DeleteWireCommand final : public EditCommand {
  public:
    DeleteWireCommand(SchematicDocument* doc, IndexedEntity<Wire> wire,
                      std::vector<IndexedEntity<Junction>> junctions)
        : EditCommand(doc), wire_(std::move(wire)),
          junctions_(std::move(junctions)) {}

    void apply() override {
        (void)take_wire(schem(), wire_.value.id);
        for (const auto& j : junctions_) {
            (void)take_junction(schem(), j.value.id);
        }
        schem().bump();
    }
    void undo() override {
        for (const auto& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        reinsert_wire(schem(), wire_);
    }
    [[nodiscard]] std::string name() const override { return "Delete Wire"; }

  private:
    IndexedEntity<Wire> wire_;
    std::vector<IndexedEntity<Junction>> junctions_;
};

class MoveJunctionCommand final : public EditCommand {
  public:
    // tap_add/tap_remove: the binding change the landing makes - a
    // junction DRAGGED onto a wire's body taps that wire (the spec's
    // "move the wire directly" case: the author's hand put it there),
    // and a tap whose wire no longer passes through the landing is
    // released (a binding whose geometry is gone would keep a stale
    // connection alive). Both computed by the pusher against the
    // landing; undo restores them exactly.
    MoveJunctionCommand(SchematicDocument& doc, std::uint32_t id,
                        ws::circsim::Pos from, ws::circsim::Pos to,
                        std::vector<WireRoute> routes,
                        std::vector<WireRoute> undo_routes,
                        std::vector<std::uint32_t> tap_add = {},
                        std::vector<std::uint32_t> tap_remove = {})
        : EditCommand(&doc), id_(id), from_(from), to_(to),
          routes_(std::move(routes)), undo_routes_(std::move(undo_routes)),
          tap_add_(std::move(tap_add)),
          tap_remove_(std::move(tap_remove)) {}

    void apply() override {
        land(to_);
        land_taps(tap_add_, true);
        land_taps(tap_remove_, false);
        land_routes(routes_);
    }
    void undo() override {
        land(from_);
        land_taps(tap_add_, false);
        land_taps(tap_remove_, true);
        land_routes(undo_routes_);
    }
    [[nodiscard]] std::string name() const override {
        return "Move Junction";
    }

  private:
    // add=true pushes the id into the dot's taps (deduped); add=false
    // erases it. Never touches a wire's own endpoint refs.
    void land_taps(const std::vector<std::uint32_t>& taps, bool add) {
        for (Junction& j : schem().junctions_mut()) {
            if (j.id != id_) {
                continue;
            }
            for (std::uint32_t t : taps) {
                const auto at =
                    std::find(j.taps.begin(), j.taps.end(), t);
                if (add && at == j.taps.end()) {
                    j.taps.push_back(t);
                } else if (!add && at != j.taps.end()) {
                    j.taps.erase(at);
                }
            }
            schem().bump();
            return;
        }
    }
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
    std::vector<std::uint32_t> tap_add_, tap_remove_;
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
    const auto indexed_wire = capture_positions(
        s.wires(), one, [](const Wire& w) { return w.id; });
    const auto junction_values = orphaned_junctions(s, one);
    const auto indexed_junctions = capture_positions(
        s.junctions(), junction_values,
        [](const Junction& j) { return j.id; });
    if (!indexed_wire.empty()) {
        (void)doc.undo().push(std::make_unique<DeleteWireCommand>(
            &doc, indexed_wire.front(), indexed_junctions));
    }
}

std::size_t push_contact_junctions(
    SchematicDocument& doc,
    const std::vector<ws::circsim::MovedPinContact>& contacts,
    std::string name) {
    // One junction per contact, minted fresh ids (0 is the "none"
    // sentinel; reinsert_junction rides the id it is handed). A
    // junction already at the spot was filtered upstream (the dot
    // exists - the contact is explicit); here every position mints.
    // Each minted dot carries its tap: the contact's own wire, the
    // authored binding the move's landing just made visible (the
    // pin's own wire ends on the dot's position through its endpoint
    // ref - the wire tool's own gesture, made durable).
    std::vector<Junction> minted;
    minted.reserve(contacts.size());
    for (const ws::circsim::MovedPinContact& c : contacts) {
        if (doc.schem.find_wire(c.wire) == nullptr) {
            continue;  // the contact's wire is gone: nothing to name
        }
        Junction j{};
        j.id = doc.schem.alloc_id();
        j.pos = c.pos;
        j.taps.push_back(c.wire);
        minted.push_back(j);
    }
    if (minted.empty()) {
        return 0;
    }
    const std::size_t count = minted.size();
    (void)doc.undo().push(std::make_unique<ContactJunctionsCommand>(
        &doc, std::move(minted), std::move(name)));
    return count;
}

void push_move_junction(SchematicDocument& doc, std::uint32_t junction_id,
                        ws::circsim::Pos to,
                        std::vector<WireRoute> wire_routes,
                        std::vector<std::uint32_t> tap_add,
                        std::vector<std::uint32_t> tap_remove) {
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
                std::move(undo_routes), std::move(tap_add),
                std::move(tap_remove)));
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
    DeleteLabelCommand(
        SchematicDocument* doc,
        IndexedEntity<ws::circsim::Label> label)
        : EditCommand(doc), label_(std::move(label)) {}

    void apply() override {
        (void)take_label(schem(), label_.value.id);
        schem().bump();
    }
    void undo() override { reinsert_label(schem(), label_); }
    [[nodiscard]] std::string name() const override { return "Delete Label"; }

  private:
    IndexedEntity<ws::circsim::Label> label_;
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
            const auto indexed = capture_positions(
                doc.schem.labels(), std::vector<ws::circsim::Label>{l},
                [](const ws::circsim::Label& item) { return item.id; });
            if (!indexed.empty()) {
                (void)doc.undo().push(
                    std::make_unique<DeleteLabelCommand>(&doc, indexed.front()));
            }
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

    // The directive text rides into the exported deck, so every landing
    // (apply, undo, redo) must bump: the version is what marks results,
    // the Problems list and the Netlist view stale.
    void apply() override { land(next_); }
    void undo() override { land(was_); }
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
    void land(const std::string& text) {
        schem().directives = text;
        schem().bump();
    }

    std::string was_, next_;
};

// The Simulation tab's temperature row (§7.7): nullopt = SPICE's 27°C
// default. Merges like the directives row does - typing "12" then "5"
// is one Edit Temperature step, not two.
class SimTempCommand final : public EditCommand {
  public:
    SimTempCommand(SchematicDocument* doc,
                   std::optional<double> was, std::optional<double> next)
        : EditCommand(doc), was_(was), next_(next) {}

    // `temp` reaches the run (RunSettings / the .options card), so it bumps
    // like every other edit - a results table computed at 27 C must not
    // read as current after the row says 85.
    void apply() override { land(next_); }
    void undo() override { land(was_); }
    [[nodiscard]] std::string name() const override {
        return "Edit Temperature";
    }
    [[nodiscard]] bool merge_with(
        const ws::app::UndoCommand& newer) override {
        const auto* edit = dynamic_cast<const SimTempCommand*>(&newer);
        if (edit == nullptr) {
            return false;
        }
        next_ = edit->next_;
        return true;
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this);
    }

  private:
    void land(std::optional<double> temp) {
        schem().temp = temp;
        schem().bump();
    }

    std::optional<double> was_, next_;
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

// The reorder the D3 list rides: one entry moves from `from` to `to`
// (an adjacent swap in practice, but the general spelling is the
// honest one - the undo mirrors it exactly). Out-of-range on either
// end refuses as a no-op, the analysis commands' own rule; a same-slot
// move is nothing and never pushes.
class MoveAnalysisCommand final : public EditCommand {
  public:
    MoveAnalysisCommand(SchematicDocument& doc, std::size_t from,
                        std::size_t to)
        : EditCommand(&doc), from_(from), to_(to) {}

    void apply() override { move(from_, to_); }
    void undo() override { move(to_, from_); }
    [[nodiscard]] std::string name() const override {
        return "Reorder Analyses";
    }

  private:
    // The vector's own rotation: erase at `from`, insert at `to` -
    // indices after the erase shift by one when to > from, exactly the
    // std::vector::insert contract; the undo's mirrored pair lands the
    // entry back on its own slot, so the two spells round-trip.
    void move(std::size_t from, std::size_t to) {
        auto& entries = schem().analysis.entries;
        if (from >= entries.size() || to >= entries.size() || from == to) {
            return;
        }
        ws::circsim::AnalysisSpec held = entries[from];
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(from));
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(to),
                       held);
        schem().bump();
    }
    std::size_t from_, to_;
};

}  // namespace

void push_directives(SchematicDocument& doc, std::string next) {
    if (doc.schem.directives == next) {
        return;
    }
    (void)doc.undo().push(std::make_unique<DirectivesCommand>(
        &doc, doc.schem.directives, std::move(next)));
}

void push_set_temp(SchematicDocument& doc, std::optional<double> next) {
    if (doc.schem.temp == next) {
        return;
    }
    (void)doc.undo().push(
        std::make_unique<SimTempCommand>(&doc, doc.schem.temp, next));
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

void push_move_analysis(SchematicDocument& doc, std::size_t from,
                        std::size_t to) {
    // The reorder's own guard, the family's rule: out of range or a
    // same-slot move refuses as a no-op push (nothing to undo).
    auto& entries = doc.schem.analysis.entries;
    if (from < entries.size() && to < entries.size() && from != to) {
        (void)doc.undo().push(
            std::make_unique<MoveAnalysisCommand>(doc, from, to));
    }
}
// next_ref lives in ws::circsim now (geom.hpp, stage 11 - the lib
// extraction); document.hpp's using-declaration keeps the editor's
// unqualified spelling resolving.

// --- the group commands (document.hpp: the box-select's landing gear) --------

namespace {

// One group move: every member lands its `to` (undo: back to its
// `from`), the wire routes riding beside. A member the schematic no
// longer finds is skipped - the honest shape, the single moves' rule.
// The glue rides as its own move list (junction ids with from/to
// positions, already resolved by the pusher): a glued junction whose
// part is ALSO in the set would double-land (the part's own pin moves,
// the glue rides it), so the canvas filters those out before pushing.
class MoveSetCommand final : public EditCommand {
  public:
    MoveSetCommand(SchematicDocument* doc, std::vector<EntityMove> moves,
                   std::vector<WireRoute> routes,
                   std::vector<WireRoute> undo_routes,
                   std::vector<GluedJunctionMove> glue = {})
        : EditCommand(doc), moves_(std::move(moves)),
          routes_(std::move(routes)), undo_routes_(std::move(undo_routes)),
          glue_(std::move(glue)) {}

    void apply() override {
        land(moves_, true);
        land_glue(true);
        (void)land_wire_routes(schem(), routes_);
    }
    void undo() override {
        land(moves_, false);
        land_glue(false);
        (void)land_wire_routes(schem(), undo_routes_);
    }
    [[nodiscard]] std::string name() const override { return "Move Set"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + moves_.size() * sizeof(EntityMove) +
               routes_bytes(routes_) + routes_bytes(undo_routes_);
    }

  private:
    // The glued junctions land their to (undo: their from) - the
    // same step as the members so one undo reverts everything.
    void land_glue(bool forward) {
        for (const GluedJunctionMove& g : glue_) {
            for (Junction& j : schem().junctions_mut()) {
                if (j.id == g.junction) {
                    j.pos = forward ? g.to : g.from;
                    break;
                }
            }
        }
    }
    void land(const std::vector<EntityMove>& moves, bool forward) {
        for (const EntityMove& m : moves) {
            const ws::circsim::Pos at = forward ? m.to : m.from;
            switch (m.kind) {
                case EntityMove::Kind::Part:
                    if (Part* p = schem().find_part(m.id)) {
                        p->pos = at;
                    }
                    break;
                case EntityMove::Kind::Junction:
                    // find_junction answers const only (the model's
                    // own seam); the mut walk is the writer's path.
                    for (Junction& j : schem().junctions_mut()) {
                        if (j.id == m.id) {
                            j.pos = at;
                            break;
                        }
                    }
                    break;
                case EntityMove::Kind::Label:
                    for (ws::circsim::Label& l : schem().labels_mut()) {
                        if (l.id == m.id) {
                            l.pos = at;
                            break;
                        }
                    }
                    break;
            }
        }
        schem().bump();
    }

    std::vector<EntityMove> moves_;
    std::vector<WireRoute> routes_, undo_routes_;
    std::vector<GluedJunctionMove> glue_;
};

// The paste: the payload's ids are pre-minted, so both directions are
// the plain container writers - apply inserts everything, undo takes
// exactly those ids back out (wires first: they name the parts and
// junctions, the delete cascade's own order).
class PasteCommand final : public EditCommand {
  public:
    PasteCommand(SchematicDocument* doc, PastePayload payload)
        : EditCommand(doc), payload_(std::move(payload)) {}

    void apply() override {
        for (const Part& p : payload_.parts) {
            reinsert_part(schem(), p);
        }
        for (const Junction& j : payload_.junctions) {
            reinsert_junction(schem(), j);
        }
        for (const Wire& w : payload_.wires) {
            reinsert_wire(schem(), w);
        }
        for (const ws::circsim::Label& l : payload_.labels) {
            reinsert_label(schem(), l);
        }
    }
    void undo() override {
        for (const Wire& w : payload_.wires) {
            (void)take_wire(schem(), w.id);
        }
        for (const Part& p : payload_.parts) {
            (void)take_part(schem(), p.id);
        }
        for (const Junction& j : payload_.junctions) {
            (void)take_junction(schem(), j.id);
        }
        for (const ws::circsim::Label& l : payload_.labels) {
            (void)take_label(schem(), l.id);
        }
        schem().bump();
    }
    [[nodiscard]] std::string name() const override { return "Paste"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + payload_.parts.size() * sizeof(Part) +
               payload_.wires.size() * sizeof(Wire) +
               payload_.junctions.size() * sizeof(Junction) +
               payload_.labels.size() * sizeof(ws::circsim::Label);
    }

  private:
    PastePayload payload_;
};

// The set delete's captured removal: the parts with their cascades,
// the wires with theirs, the labels - all as the ONE step's payload.
class DeleteSetCommand final : public EditCommand {
  public:
    DeleteSetCommand(
        SchematicDocument* doc, std::vector<IndexedEntity<Part>> parts,
        std::vector<IndexedEntity<Wire>> wires,
        std::vector<IndexedEntity<Junction>> junctions,
        std::vector<IndexedEntity<ws::circsim::Label>> labels)
        : EditCommand(doc), parts_(std::move(parts)), wires_(std::move(wires)),
          junctions_(std::move(junctions)), labels_(std::move(labels)) {}

    void apply() override {
        for (const auto& w : wires_) {
            (void)take_wire(schem(), w.value.id);
        }
        for (const auto& p : parts_) {
            (void)take_part(schem(), p.value.id);
        }
        for (const auto& l : labels_) {
            (void)take_label(schem(), l.value.id);
        }
        for (const auto& j : junctions_) {
            (void)take_junction(schem(), j.value.id);
        }
        schem().bump();
    }
    void undo() override {
        for (const auto& p : parts_) {
            reinsert_part(schem(), p);
        }
        for (const auto& l : labels_) {
            reinsert_label(schem(), l);
        }
        for (const auto& j : junctions_) {
            reinsert_junction(schem(), j);
        }
        for (const auto& w : wires_) {
            reinsert_wire(schem(), w);
        }
    }
    [[nodiscard]] std::string name() const override { return "Delete Set"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(*this) + parts_.size() * sizeof(IndexedEntity<Part>) +
               wires_.size() * sizeof(IndexedEntity<Wire>) +
               junctions_.size() * sizeof(IndexedEntity<Junction>) +
               labels_.size() *
                   sizeof(IndexedEntity<ws::circsim::Label>);
    }

  private:
    std::vector<IndexedEntity<Part>> parts_;
    std::vector<IndexedEntity<Wire>> wires_;
    std::vector<IndexedEntity<Junction>> junctions_;
    std::vector<IndexedEntity<ws::circsim::Label>> labels_;
};

}  // namespace

void push_move_set(SchematicDocument& doc, std::vector<EntityMove> moves,
                   std::vector<WireRoute> routes,
                   std::vector<GluedJunctionMove> glue) {
    if (moves.empty()) {
        return;
    }
    // The undo shape: every routed wire's CURRENT waypoints (the
    // single move's own capture, widened to the set).
    std::vector<WireRoute> undo_routes;
    undo_routes.reserve(routes.size());
    for (const WireRoute& r : routes) {
        if (const Wire* w = doc.schem.find_wire(r.wire)) {
            undo_routes.push_back(WireRoute{r.wire, w->waypoints});
        }
    }
    (void)doc.undo().push(std::make_unique<MoveSetCommand>(
        &doc, std::move(moves), std::move(routes),
        std::move(undo_routes), std::move(glue)));
}

void push_paste(SchematicDocument& doc, PastePayload payload) {
    if (payload.parts.empty() && payload.junctions.empty() &&
        payload.wires.empty() && payload.labels.empty()) {
        return;
    }
    (void)doc.undo().push(
        std::make_unique<PasteCommand>(&doc, std::move(payload)));
}

bool push_delete_set(SchematicDocument& doc,
                     const std::vector<std::uint32_t>& parts,
                     const std::vector<std::uint32_t>& wires,
                     const std::vector<std::uint32_t>& labels) {
    const Schematic& s = doc.schem;

    // The wires the removal takes: the named ones plus every wire
    // that references a named part's pin (the part rule, widened -
    // deduped by id so a named wire is not captured twice).
    std::vector<Wire> taking;
    taking.reserve(wires.size());
    std::set<std::uint32_t> taken_ids;
    const auto take_wire_by_id = [&](std::uint32_t id) {
        if (taken_ids.insert(id).second) {
            if (const Wire* w = s.find_wire(id)) {
                taking.push_back(*w);
            }
        }
    };
    for (std::uint32_t id : wires) {
        take_wire_by_id(id);
    }
    for (std::uint32_t part_id : parts) {
        for (const Wire& w : s.wires()) {
            const PinRef* a = std::get_if<PinRef>(&w.a);
            const PinRef* b = std::get_if<PinRef>(&w.b);
            if ((a != nullptr && a->part == part_id) ||
                (b != nullptr && b->part == part_id)) {
                take_wire_by_id(w.id);
            }
        }
    }

    // The parts and labels themselves (skipping ids the schematic no
    // longer has - the set is honest, but honest costs nothing here).
    std::vector<Part> part_copies;
    for (std::uint32_t id : parts) {
        if (const Part* p = s.find_part(id)) {
            part_copies.push_back(*p);
        }
    }
    std::vector<ws::circsim::Label> label_copies;
    for (std::uint32_t id : labels) {
        if (const ws::circsim::Label* l = s.find_label(id)) {
            label_copies.push_back(*l);
        }
    }
    if (part_copies.empty() && taking.empty() && label_copies.empty()) {
        return false;
    }

    // The junctions the wire removals orphan (the wire rule: kept
    // when any surviving wire still wants them).
    std::vector<Junction> junctions = orphaned_junctions(s, taking);
    const auto indexed_parts = capture_positions(
        s.parts(), part_copies, [](const Part& p) { return p.id; });
    const auto indexed_wires = capture_positions(
        s.wires(), taking, [](const Wire& w) { return w.id; });
    const auto indexed_junctions = capture_positions(
        s.junctions(), junctions, [](const Junction& j) { return j.id; });
    const auto indexed_labels = capture_positions(
        s.labels(), label_copies,
        [](const ws::circsim::Label& l) { return l.id; });
    (void)doc.undo().push(std::make_unique<DeleteSetCommand>(
        &doc, indexed_parts, indexed_wires, indexed_junctions, indexed_labels));
    return true;
}

}  // namespace editor
