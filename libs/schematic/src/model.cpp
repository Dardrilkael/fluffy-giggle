// ws::sch - the model + nets implementation (phase 1). Everything here
// is pure std over the graph: no geometry ever decides connectivity.

#include <ws/schematic/model.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "geom.hpp"

namespace ws::sch {

// --- Doc: find -----------------------------------------------------------------

const Node* Doc::find_node(NodeId id) const {
    return detail::find_by_id(nodes, id);
}
Node* Doc::find_node(NodeId id) { return detail::find_by_id(nodes, id); }
const Wire* Doc::find_wire(WireId id) const {
    return detail::find_by_id(wires, id);
}
Wire* Doc::find_wire(WireId id) { return detail::find_by_id(wires, id); }
const Label* Doc::find_label(LabelId id) const {
    return detail::find_by_id(labels, id);
}

// --- Doc: builders ---------------------------------------------------------------

NodeId Doc::add_pin(PartId part, std::uint32_t pin, Vec2 pos) {
    Node n;
    n.id = alloc_node();
    n.kind = NodeKind::Pin;
    n.part = part;
    n.pin = pin;
    n.pos = pos;
    nodes.push_back(n);
    return n.id;
}

NodeId Doc::add_junction(Vec2 pos) {
    Node n;
    n.id = alloc_node();
    n.kind = NodeKind::Junction;
    n.pos = pos;
    nodes.push_back(n);
    return n.id;
}

LabelId Doc::add_label(Vec2 pos, std::string name, WireId wire) {
    Label l;
    l.id = alloc_label();
    l.pos = pos;
    l.name = std::move(name);
    l.wire = wire;
    labels.push_back(std::move(l));
    return labels.back().id;
}

WireId Doc::add_wire(NodeId a, NodeId b, std::vector<Vec2> mid, int a_dir,
                     int b_dir) {
    Wire w;
    w.id = alloc_wire();
    w.a = a;
    w.b = b;
    w.mid = std::move(mid);
    w.a_dir = a_dir;
    w.b_dir = b_dir;
    wires.push_back(std::move(w));
    return wires.back().id;
}

// --- Doc: the split / merge (the ONLY T-connection) -------------------------------

WireId Doc::restore_wire(const Wire& w) {
    // Sorted insert: an undo puts the wire back where it was in the
    // sequence, not at the end.
    detail::insert_by_id(wires, w);
    raise_wire_counter(w.id);
    return w.id;
}

std::optional<Doc::Split> Doc::split_wire(WireId id, Vec2 pos) {
    return split_wire(id, pos, SplitIds{});
}

std::optional<Doc::Split> Doc::split_wire(WireId id, Vec2 pos, SplitIds ids) {
    using detail::near;
    using detail::on_segment;

    Wire* w = find_wire(id);
    if (w == nullptr) {
        return std::nullopt;
    }
    const std::vector<Vec2> path = wire_path(id);
    if (path.size() < 2u) {
        return std::nullopt;  // dangling: nothing to split
    }
    // An endpoint's own position is not a split (the wire already ends at
    // a node there).
    if (near(pos, path.front()) || near(pos, path.back())) {
        return std::nullopt;
    }
    // The segment carrying the point (path[seg]..path[seg+1]); none = the
    // point is off the path.
    std::size_t seg = path.size();
    for (std::size_t k = 1; k < path.size(); ++k) {
        if (on_segment(path[k - 1], path[k], pos)) {
            seg = k - 1;
            break;
        }
    }
    if (seg == path.size()) {
        return std::nullopt;
    }

    // The junction node at the split point.
    Node j;
    j.id = ids.junction != 0 ? ids.junction : alloc_node();
    raise_node_counter(j.id);
    j.kind = NodeKind::Junction;
    j.pos = pos;

    // One group id for every piece cut from the same conductor, so a
    // chain of splits can be merged back junction by junction.
    const WireId group = w->split_of != 0 ? w->split_of : w->id;

    // First half: the a side, KEEPING the original id (labels and undo
    // records that name this wire stay valid). Its b end is the junction:
    // a junction has no pin geometry, so that end is free.
    Wire first = *w;
    first.b = j.id;
    first.b_dir = kDirFree;
    first.split_of = group;
    first.mid.clear();

    Wire second;
    second.id = ids.second != 0 ? ids.second : alloc_wire();
    raise_wire_counter(second.id);
    second.a = j.id;
    second.b = w->b;
    second.a_dir = kDirFree;
    second.b_dir = w->b_dir;
    second.split_of = group;

    // Split the interior waypoints: vertices up to the carrying segment go
    // to the first half, the rest to the second. A vertex exactly AT the
    // point belongs to neither - it became the junction.
    for (std::size_t k = 1; k + 1 < path.size(); ++k) {
        if (near(path[k], pos)) {
            continue;
        }
        if (k <= seg) {
            first.mid.push_back(path[k]);
        } else {
            second.mid.push_back(path[k]);
        }
    }

    const Split result{j.id, first.id, second.id};
    detail::insert_by_id(nodes, j);  // different vector: `w` stays valid
    *w = std::move(first);           // last use of `w`: the insert below may move it
    detail::insert_by_id(wires, std::move(second));
    return result;
}

WireId Doc::merge_split(NodeId jid, bool allow_any) {
    const Node* j = find_node(jid);
    if (j == nullptr || j->kind != NodeKind::Junction) {
        return 0;
    }
    // Exactly two wires, both ending at the junction.
    const Wire* wa = nullptr;
    const Wire* wb = nullptr;
    for (const Wire& w : wires) {
        if (w.a == jid || w.b == jid) {
            if (wa == nullptr) {
                wa = &w;
            } else if (wb == nullptr) {
                wb = &w;
            } else {
                return 0;  // three or more: a real junction, not a seam
            }
        }
    }
    if (wa == nullptr || wb == nullptr) {
        return 0;
    }
    if (!allow_any && (wa->split_of == 0 || wa->split_of != wb->split_of)) {
        return 0;  // not a split pair
    }

    // Orient the pair so `p` runs INTO the junction and `q` OUT of it
    // (an allow_any dot may have been drawn with both wires pointing the
    // same way; flip one so the merge never keeps the removed junction as
    // an endpoint).
    Wire p = *wa;
    Wire q = *wb;
    const auto flip = [](Wire& w) {
        std::swap(w.a, w.b);
        std::swap(w.a_dir, w.b_dir);
        std::reverse(w.mid.begin(), w.mid.end());
    };
    const bool p_in = p.b == jid;
    const bool q_in = q.b == jid;
    if (!p_in && q_in) {
        std::swap(p, q);
    } else if (p_in && q_in) {
        flip(q);
    } else if (!p_in && !q_in) {
        flip(p);
    }

    // The merged wire keeps p's id. The junction's position stays as a
    // waypoint (the shape the author drew through the split); the
    // router's cleanup folds it when it is collinear.
    Wire merged = p;
    merged.b = q.b;
    merged.b_dir = q.b_dir;
    merged.mid.push_back(j->pos);  // `j` is read here, BEFORE the erase below
    merged.mid.insert(merged.mid.end(), q.mid.begin(), q.mid.end());

    const WireId keep = p.id;
    const WireId gone = q.id;
    const WireId group = p.split_of == q.split_of ? p.split_of : 0;

    detail::erase_by_id(wires, gone);  // invalidates wa / wb / j: not used again
    detail::erase_by_id(nodes, jid);
    Wire* kept = find_wire(keep);
    *kept = std::move(merged);

    // Labels bound to the absorbed half now name the merged wire.
    for (Label& l : labels) {
        if (l.wire == gone) {
            l.wire = keep;
        }
    }
    // The group id survives only while other pieces of it remain.
    const bool others = group != 0 && std::any_of(
        wires.begin(), wires.end(),
        [&](const Wire& w) { return w.id != keep && w.split_of == group; });
    kept->split_of = others ? group : 0;
    return keep;
}

bool Doc::erase_node(NodeId id) { return detail::erase_by_id(nodes, id); }

bool Doc::erase_wire(WireId id) { return detail::erase_by_id(wires, id); }

std::vector<Vec2> Doc::wire_path(WireId id) const {
    const Wire* w = find_wire(id);
    if (w == nullptr) {
        return {};
    }
    const Node* a = find_node(w->a);
    const Node* b = find_node(w->b);
    if (a == nullptr || b == nullptr) {
        return {};  // dangling: nothing to draw or route
    }
    std::vector<Vec2> path;
    path.reserve(w->mid.size() + 2u);
    path.push_back(a->pos);
    path.insert(path.end(), w->mid.begin(), w->mid.end());
    path.push_back(b->pos);
    return path;
}

// --- nets ------------------------------------------------------------------------

namespace {

class Dsu {
public:
    explicit Dsu(std::size_t n) : parent_(n), rank_(n, 0) {
        for (std::size_t i = 0; i < n; ++i) {
            parent_[i] = i;
        }
    }
    std::size_t find(std::size_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (rank_[a] < rank_[b]) {
            std::swap(a, b);
        }
        parent_[b] = a;
        if (rank_[a] == rank_[b]) {
            ++rank_[a];
        }
    }

private:
    std::vector<std::size_t> parent_;
    std::vector<std::uint8_t> rank_;
};

[[nodiscard]] std::string to_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

NetList build_nets(const Doc& doc) {
    using detail::index_by_id;
    using detail::kNpos;

    // Slots: one per node (its index), then one per BOUND label. Unbound
    // labels are DECORATIONS - no slot, they join no net.
    constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
    const std::size_t n_nodes = doc.nodes.size();
    std::vector<std::size_t> label_slot(doc.labels.size(), kNoSlot);
    std::size_t slots = n_nodes;
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        if (doc.labels[i].wire != 0) {
            label_slot[i] = slots++;
        }
    }
    Dsu dsu(slots);

    // Wires join their endpoint nodes (a dangling wire joins nothing).
    for (const Wire& w : doc.wires) {
        const std::size_t a = index_by_id(doc.nodes, w.a);
        const std::size_t b = index_by_id(doc.nodes, w.b);
        if (a != kNpos && b != kNpos) {
            dsu.unite(a, b);
        }
    }

    // Bound labels join the wire's a-end node; same-name labels merge.
    // (The binding is by WIRE ID - geometry never enters: the label does
    // not need to sit on the wire to name it.)
    std::unordered_map<std::string, std::size_t> by_lower;
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        const Label& l = doc.labels[i];
        if (l.wire == 0) {
            continue;
        }
        const Wire* w = doc.find_wire(l.wire);
        if (w == nullptr) {
            continue;  // bound to a deleted wire: a dangling name
        }
        const std::size_t a = index_by_id(doc.nodes, w->a);
        if (a != kNpos) {
            dsu.unite(label_slot[i], a);
        }
        const auto [it, fresh] =
            by_lower.try_emplace(to_lower(l.name), label_slot[i]);
        if (!fresh) {
            dsu.unite(label_slot[i], it->second);
        }
    }

    // Collect per component. Ids within a net are sorted; nets are ordered
    // by their first id (the contract in model.hpp), independent of DSU
    // internals and of the storage order of doc.nodes.
    std::vector<std::vector<NodeId>> by_root(slots);
    for (std::size_t i = 0; i < n_nodes; ++i) {
        by_root[dsu.find(i)].push_back(doc.nodes[i].id);
    }
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        if (label_slot[i] != kNoSlot) {
            by_root[dsu.find(label_slot[i])].push_back(label_marker(i));
        }
    }
    NetList out;
    for (auto& ids : by_root) {
        if (!ids.empty()) {
            std::sort(ids.begin(), ids.end());
            out.nets.push_back(std::move(ids));
        }
    }
    std::sort(out.nets.begin(), out.nets.end(),
              [](const auto& x, const auto& y) { return x.front() < y.front(); });
    // Nets with a single UNBOUND node are still nets (a dot alone) - the
    // editor's cleanup policy, not the net builder's, drops those.
    return out;
}

const std::vector<NodeId>* NetList::net_of(NodeId n) const {
    for (const auto& net : nets) {
        if (std::binary_search(net.begin(), net.end(), n)) {
            return &net;
        }
    }
    return nullptr;
}

std::vector<PinPair> pin_connections(const Doc& doc) {
    const NetList nets = build_nets(doc);
    std::vector<PinPair> out;
    std::vector<NodeId> pins;
    for (const auto& net : nets.nets) {
        pins.clear();
        for (const NodeId id : net) {
            if (is_label_marker(id)) {
                continue;
            }
            const Node* n = doc.find_node(id);
            if (n != nullptr && n->is_pin()) {
                pins.push_back(id);
            }
        }
        // `pins` is sorted (net ids are), so each pair is already ordered
        // (first < second) and distinct across nets (a pin is in one net).
        for (std::size_t i = 0; i < pins.size(); ++i) {
            for (std::size_t j = i + 1; j < pins.size(); ++j) {
                out.emplace_back(pins[i], pins[j]);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace ws::sch
