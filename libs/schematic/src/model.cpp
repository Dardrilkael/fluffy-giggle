// ws::sch - the model + nets implementation (phase 1). Everything here
// is pure std over the graph: no geometry ever decides connectivity.

#include <ws/schematic/model.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

#include "geom.hpp"

namespace ws::sch {

// --- Doc: find -----------------------------------------------------------------

const Node* Doc::find_node(NodeId id) const {
    for (const Node& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

Node* Doc::find_node(NodeId id) {
    for (Node& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

const Wire* Doc::find_wire(WireId id) const {
    for (const Wire& w : wires) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

Wire* Doc::find_wire(WireId id) {
    for (Wire& w : wires) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

const Label* Doc::find_label(LabelId id) const {
    for (const Label& l : labels) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
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
    labels.push_back(l);
    return l.id;
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
    wires.push_back(w);
    return w.id;
}

// --- Doc: the split / merge (the ONLY T-connection) -------------------------------

WireId Doc::restore_wire(const Wire& w) {
    wires.push_back(w);
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

    const WireId first_id = first.id;
    const WireId second_id = second.id;
    nodes.push_back(j);
    *w = first;  // w is not used after this line (push_back below may move it)
    wires.push_back(std::move(second));
    return Split{j.id, first_id, second_id};
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
    merged.mid.push_back(j->pos);
    merged.mid.insert(merged.mid.end(), q.mid.begin(), q.mid.end());

    const WireId keep = p.id;
    const WireId gone = q.id;
    const WireId group = p.split_of == q.split_of ? p.split_of : 0;

    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&](const Wire& w) { return w.id == gone; }),
                wires.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](const Node& n) { return n.id == jid; }),
                nodes.end());
    *find_wire(keep) = merged;

    // Labels bound to the absorbed half now name the merged wire.
    for (Label& l : labels) {
        if (l.wire == gone) {
            l.wire = keep;
        }
    }
    // The group id survives only while other pieces of it remain.
    const bool others = std::any_of(
        wires.begin(), wires.end(), [&](const Wire& w) {
            return w.id != keep && group != 0 && w.split_of == group;
        });
    find_wire(keep)->split_of = others ? group : 0;
    return keep;
}

bool Doc::erase_node(NodeId id) {
    const auto it = std::remove_if(
        nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == id; });
    if (it == nodes.end()) {
        return false;
    }
    nodes.erase(it, nodes.end());
    return true;
}

bool Doc::erase_wire(WireId id) {
    const auto it = std::remove_if(
        wires.begin(), wires.end(),
        [&](const Wire& w) { return w.id == id; });
    if (it == wires.end()) {
        return false;
    }
    wires.erase(it, wires.end());
    return true;
}

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
    std::vector<std::size_t> rank_;
};

}  // namespace

NetList build_nets(const Doc& doc) {
    // One slot per node (labels get their own slots through the binding
    // pass below - a label node joins the graph only when bound).
    std::vector<std::size_t> node_slot(doc.nodes.size(), doc.nodes.size());
    for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
        node_slot[i] = i;
    }
    // Labels: unbound labels are DECORATIONS (no slot of their own - they
    // join no net); bound labels get a slot past the nodes. The "no slot"
    // sentinel must be a value no slot can take: it used to be
    // nodes.size(), which is exactly the FIRST bound label's slot, so that
    // label was silently dropped from every net.
    constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> label_slot(doc.labels.size(), kNoSlot);
    std::size_t slots = doc.nodes.size();
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        if (doc.labels[i].wire != 0) {
            label_slot[i] = slots++;
        }
    }
    Dsu dsu(slots);

    const auto node_index = [&](NodeId id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
            if (doc.nodes[i].id == id) {
                return i;
            }
        }
        return std::nullopt;
    };

    // Wires join their endpoint nodes.
    for (const Wire& w : doc.wires) {
        const auto a = node_index(w.a);
        const auto b = node_index(w.b);
        if (!a || !b) {
            continue;  // dangling: joins nothing
        }
        dsu.unite(*a, *b);
    }

    // Bound labels join the wire's a-end node; same-name labels merge.
    // (The binding is by WIRE ID - geometry never enters: the label does
    // not need to sit on the wire to name it.)
    std::map<std::string, std::size_t> by_lower;
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        const Label& l = doc.labels[i];
        if (l.wire == 0) {
            continue;
        }
        const Wire* w = doc.find_wire(l.wire);
        if (w == nullptr) {
            continue;  // bound to a deleted wire: a dangling name
        }
        const auto a = node_index(w->a);
        if (a) {
            dsu.unite(label_slot[i], *a);
        }
        std::string low;
        for (char c : l.name) {
            low.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        const auto it = by_lower.find(low);
        if (it != by_lower.end()) {
            dsu.unite(label_slot[i], it->second);
        } else {
            by_lower.emplace(std::move(low), label_slot[i]);
        }
    }

    // Collect: slot -> node ids (labels joined the component their slot
    // rides with).
    std::map<std::size_t, std::vector<NodeId>> comps;
    for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
        comps[dsu.find(node_slot[i])].push_back(doc.nodes[i].id);
    }
    for (std::size_t i = 0; i < doc.labels.size(); ++i) {
        if (label_slot[i] != kNoSlot) {
            comps[dsu.find(label_slot[i])].push_back(
                static_cast<NodeId>(0x8000'0000u | i));  // label marker
        }
    }

    NetList out;
    for (auto& [root, ids] : comps) {
        std::sort(ids.begin(), ids.end());
        out.nets.push_back(std::move(ids));
    }
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
        for (NodeId id : net) {
            if ((id & 0x8000'0000u) != 0u) {
                continue;  // a label slot marker
            }
            const Node* n = doc.find_node(id);
            if (n != nullptr && n->is_pin()) {
                pins.push_back(id);
            }
        }
        for (std::size_t i = 0; i < pins.size(); ++i) {
            for (std::size_t j = i + 1; j < pins.size(); ++j) {
                out.emplace_back(pins[i], pins[j]);
            }
        }
    }
    for (auto& p : out) {
        if (p.first > p.second) {
            std::swap(p.first, p.second);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace ws::sch
