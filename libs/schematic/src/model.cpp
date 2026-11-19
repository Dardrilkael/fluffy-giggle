// ws::sch - the model + nets implementation (phase 1). Everything here
// is pure std over the graph: no geometry ever decides connectivity.

#include <ws/schematic/model.hpp>

#include <algorithm>
#include <cctype>

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

namespace {

constexpr double kEps = 1e-9;

bool on_segment(const Vec2& a, const Vec2& b, const Vec2& p) {
    const double cross = (b.x - a.x) * (p.y - a.y) -
                         (b.y - a.y) * (p.x - a.x);
    if (std::fabs(cross) > kEps) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) - kEps &&
           p.x <= std::max(a.x, b.x) + kEps &&
           p.y >= std::min(a.y, b.y) - kEps && p.y <= std::max(a.y, b.y) + kEps;
}

}  // namespace

std::optional<Doc::Split> Doc::split_wire(WireId id, Vec2 pos) {
    Wire* w = find_wire(id);
    if (w == nullptr) {
        return std::nullopt;
    }
    const std::vector<Vec2> path = wire_path(id);
    if (path.size() < 2u) {
        return std::nullopt;  // dangling: nothing to split
    }
    // The split point must lie on the path; an endpoint's own position is
    // not a split (the wire already ends at a node there).
    bool on = false;
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (on_segment(path[i - 1], path[i], pos)) {
            on = true;
            break;
        }
    }
    if (!on) {
        return std::nullopt;
    }
    if ((on_segment(path[0], path[1], pos) &&
         std::fabs(pos.x - path[0].x) <= kEps &&
         std::fabs(pos.y - path[0].y) <= kEps) ||
        (on_segment(path[path.size() - 2], path.back(), pos) &&
         std::fabs(pos.x - path.back().x) <= kEps &&
         std::fabs(pos.y - path.back().y) <= kEps)) {
        return std::nullopt;  // at an endpoint: that end already has a node
    }

    // The junction node at the split point.
    Node j;
    j.id = alloc_node();
    j.kind = NodeKind::Junction;
    j.pos = pos;
    nodes.push_back(j);

    // Carry the a-side's exit direction onto the first half; the second
    // half leaves the junction free (a junction has no pin geometry).
    Wire first;
    first.id = alloc_wire();
    first.a = w->a;
    first.b = j.id;
    first.a_dir = w->a_dir;
    first.b_dir = w->b_dir;  // the b-end's own approach now bends at the
                             // junction the same way it bent into the wire
    Wire second;
    second.id = alloc_wire();
    second.a = j.id;
    second.b = w->b;
    second.a_dir = kDirFree;  // leaving a junction: free
    second.b_dir = w->b_dir;
    first.split_of = w->id;
    second.split_of = w->id;

    // Split the interior waypoints at the split point: find the path
    // SEGMENT carrying the point (index k: path[k]..path[k+1]), then the
    // first half keeps vertices 1..k and the second half keeps vertices
    // k+1.. (a vertex exactly AT the point belongs to neither - it became
    // the junction).
    std::size_t seg = path.size() - 1;
    for (std::size_t k = 1; k < path.size(); ++k) {
        if (on_segment(path[k - 1], path[k], pos)) {
            seg = k - 1;
            break;
        }
    }
    for (std::size_t k = 1; k + 1 < path.size(); ++k) {
        // The vertex at the split point itself belongs to NEITHER half -
        // it became the junction (the same fold the cleanup pass runs).
        if (std::fabs(path[k].x - pos.x) <= kEps &&
            std::fabs(path[k].y - pos.y) <= kEps) {
            continue;
        }
        // vertex k belongs to the first half when it lies at or before the
        // split segment's end, else to the second half.
        if (k <= seg) {
            first.mid.push_back(path[k]);
        } else {
            second.mid.push_back(path[k]);
        }
    }

    // Replace the wire with the two halves.
    *w = first;                // reuse the slot for the first half
    wires.push_back(second);   // the lib holds no pointers across this
                               // edit (ids only), so vector is safe.
    return Split{j.id, first.id, second.id};
}

WireId Doc::merge_split(NodeId jid, bool allow_any) {
    const Node* j = find_node(jid);
    if (j == nullptr || j->kind != NodeKind::Junction) {
        return 0;
    }
    // Exactly two wires, both ending at the junction.
    Wire* wa = nullptr;
    Wire* wb = nullptr;
    for (Wire& w : wires) {
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

    // Order: the half whose a-side is the junction is the SECOND half.
    Wire* first = wa->a == jid ? wb : wa;
    Wire* second = first == wa ? wb : wa;

    Wire merged;
    merged.id = alloc_wire();
    merged.a = first->a;
    merged.b = second->b;
    merged.a_dir = first->a_dir;
    merged.b_dir = second->b_dir;
    merged.split_of = 0;
    merged.mid = first->mid;
    // The junction's position was a real corner only if it is not
    // collinear with its neighbours; the router's fold drops it when it
    // is. Keep it as a waypoint when it bends (the shape the author drew
    // through the split) - the cleanup pass settles collinearity.
    merged.mid.push_back(j->pos);
    merged.mid.insert(merged.mid.end(), second->mid.begin(), second->mid.end());

    // Drop the two halves, the junction node; add the merged wire.
    const WireId fa = first->id;
    const WireId sb = second->id;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                                [&](const Wire& w) {
                                    return w.id == fa || w.id == sb;
                                }),
                wires.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](const Node& n) { return n.id == jid; }),
                nodes.end());
    wires.push_back(merged);
    return merged.id;
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
    // join no net); bound labels get a slot past the nodes.
    std::vector<std::size_t> label_slot(doc.labels.size(),
                                        doc.nodes.size());
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
        if (label_slot[i] != doc.nodes.size()) {
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
