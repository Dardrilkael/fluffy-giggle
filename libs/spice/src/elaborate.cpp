// Elaboration (DESIGN.md §8.4, §6.6): models table, subckt flattening with
// ':'-scoped names, the node table, device creation through the factory
// table on THIS object (never a global), control resolution, and the
// topological graph checks that turn future "singular matrix" mysteries
// into named, hinted errors before any solve runs.

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cards.hpp"
#include "circuit.hpp"

namespace ws::spice {

namespace {

Diagnostic elab_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

// Node arity + controlling-source token index per device letter. The
// factories re-validate arity; this table is the flattener's token
// rewriter (which tokens are nodes vs values vs control names).
struct DeviceShape {
    std::size_t nodes = 0;
    int control_token = -1;  // index into card.rest, after the nodes
};

std::map<char, DeviceShape> device_shapes() {
    static const std::map<char, DeviceShape> table = {
        {'R', {2, -1}}, {'V', {2, -1}}, {'I', {2, -1}},
        {'E', {4, -1}}, {'G', {4, -1}},
        {'F', {2, 2}},  // F n+ n- Vname gain
        {'H', {2, 2}},  // H n+ n- Vname transresistance
    };
    return table;
}

[[nodiscard]] bool is_ground_spelling(const std::string& text) {
    const std::string lower = to_lower(text);
    return lower == "0" || lower == "gnd";
}

}  // namespace

// --- .options ------------------------------------------------------------------

ws::core::Status apply_options(const std::vector<OptionsCard>& options, RunSettings& out) {
    for (const OptionsCard& card : options) {
        for (const auto& [key, value] : card.kv) {
            const std::string lower = to_lower(key.text);
            const double v = parse_number(value.text).value_or(0.0);
            if (lower == "reltol") {
                out.reltol = v;
            } else if (lower == "abstol") {
                out.abstol = v;
            } else if (lower == "vntol") {
                out.vntol = v;
            } else if (lower == "gmin") {
                out.gmin = v;
            } else if (lower == "itl1") {
                out.itl1 = static_cast<int>(v);
            } else if (lower == "itl2") {
                out.itl2 = static_cast<int>(v);
            } else if (lower == "temp" || lower == "tnom") {
                return spice_fail(elab_diag(
                    ErrorKind::Parse, "spice.parse.unsupported_card",
                    "'.options " + lower + "' lands with temperature (M5)", card.loc));
            } else if (lower == "solver") {
                const std::string backend = to_lower(value.text);
                if (backend != "eigen") {
                    return spice_fail(elab_diag(
                        ErrorKind::Parse, "spice.parse.solver",
                        "'.options solver=" + value.text +
                            "': only the Eigen backend exists until KLU lands (M0b)",
                        card.loc));
                }
                out.solver = backend;
            } else if (lower == "gshunt") {
                return spice_fail(elab_diag(
                    ErrorKind::Parse, "spice.parse.unsupported_card",
                    "'.options gshunt' lands with the convergence ladder (M3)",
                    card.loc));
            }
        }
    }
    return {};
}

// --- the elaborator ----------------------------------------------------------------

class Elaborator final : private NodeResolver {
public:
    Elaborator(const DeckAst& ast, std::vector<Warning>& warnings)
        : ast_(ast), warnings_(warnings) {
        for (const auto& [letter, factory] : cards::builtin_device_factories()) {
            factories_[letter] = factory;
        }
    }

    ws::core::Status elaborate();
    [[nodiscard]] const Circuit& circuit() const { return circuit_; }
    [[nodiscard]] Circuit take_circuit() { return std::move(circuit_); }

private:
    // --- NodeResolver -------------------------------------------------------
    NodeId node(const Token& name) override {
        if (is_ground_spelling(name.text)) {
            return kGround;
        }
        const std::string key = to_lower(name.text);
        if (const auto it = circuit_.node_ids.find(key); it != circuit_.node_ids.end()) {
            return it->second;
        }
        const NodeId id = circuit_.node_names.size();
        circuit_.node_names.push_back(name.text);
        circuit_.node_ids.emplace(key, id);
        return id;
    }
    BranchId claim_branch() override { return circuit_.branch_count++; }

    ws::core::Status add_device_card(const DeviceCard& card, char letter,
                                    const ParamTable& params);
    ws::core::Status flatten_x(const DeviceCard& card, const ParamTable& caller_params,
                               const std::string& prefix,
                               std::vector<std::string>& subckt_stack);
    ws::core::Status resolve_controls();
    ws::core::Status check_topology();

    const DeckAst& ast_;
    std::vector<Warning>& warnings_;
    std::map<char, DeviceFactory> factories_;
    Circuit circuit_;
    ParamTable global_params_;  // deck-level .param scope (§8.4)
    std::vector<ControlRequest> pending_controls_;
    // V-source branches by lowercased device name (control resolution).
    std::map<std::string, BranchId> vsource_branches_;
};

ws::core::Status Elaborator::add_device_card(const DeviceCard& card, char letter,
                                             const ParamTable& params) {
    const auto it = factories_.find(letter);
    if (it == factories_.end()) {
        return spice_fail(elab_diag(
            ErrorKind::Parse, "spice.parse.unsupported_device",
            std::string("device '") + letter + "' is not supported yet (" +
                cards::unsupported_device_milestone(letter) + ")",
            card.loc));
    }
    ws::core::Result<std::unique_ptr<Device>> dev = it->second(card, *this, params,
                                                               pending_controls_);
    if (!dev) {
        return ws::core::unexpected(std::move(dev.error()));
    }
    if (dev.value()->has_branch()) {
        vsource_branches_[to_lower(dev.value()->name())] = dev.value()->branch();
        circuit_.branch_devices.push_back(circuit_.devices.size());
    } else if (dev.value()->kind() == 'V') {
        // V sources always have branches; kept for clarity.
    }
    circuit_.devices.push_back(std::move(*dev));
    return {};
}

ws::core::Status Elaborator::flatten_x(const DeviceCard& card,
                                       const ParamTable& caller_params,
                                       const std::string& prefix,
                                       std::vector<std::string>& subckt_stack) {
    // X name n1 n2... subname [params: k=v | k=v ...]
    if (card.rest.size() < 2) {
        return spice_fail(elab_diag(ErrorKind::Semantic, "spice.semantic.x_arity",
                                    "subcircuit call " + card.name.text +
                                        " needs nodes and a subcircuit name",
                                    card.loc));
    }
    // X name n1 n2... subname [params: k=v | k=v ...] - the subckt name is
    // the token BEFORE the first params:/k=v token, or the last token.
    std::size_t subname_idx = card.rest.size();
    std::vector<Token> instance_params_raw;
    for (std::size_t i = 1; i < card.rest.size(); ++i) {
        const std::string t = to_lower(card.rest[i].text);
        if (t == "params:" || card.rest[i].text.find('=') != std::string::npos) {
            if (i < 2) {
                return spice_fail(elab_diag(
                    ErrorKind::Semantic, "spice.semantic.x_arity",
                    "subcircuit call " + card.name.text +
                        " needs nodes before the subcircuit name", card.loc));
            }
            subname_idx = i - 1;
            for (std::size_t j = i; j < card.rest.size(); ++j) {
                if (to_lower(card.rest[j].text) == "params:") {
                    continue;
                }
                instance_params_raw.push_back(card.rest[j]);
            }
            break;
        }
    }
    if (subname_idx == card.rest.size()) {
        subname_idx = card.rest.size() - 1;  // last token is the subckt name
    }
    const Token subname = card.rest[subname_idx];
    const std::string key = to_lower(subname.text);
    const auto def_it = ast_.subckts.find(key);
    if (def_it == ast_.subckts.end()) {
        return spice_fail(elab_diag(
            ErrorKind::Semantic, "spice.semantic.unknown_subckt",
            "unknown subcircuit '" + subname.text + "'", subname.loc));
    }
    const SubcktDef& def = def_it->second;

    // Recursion: direct or mutual, named by the call chain (§8.4).
    for (const std::string& active : subckt_stack) {
        if (active == key) {
            std::string chain;
            for (const std::string& s : subckt_stack) {
                chain += (chain.empty() ? "" : " -> ") + s;
            }
            chain += " -> " + key;
            return spice_fail(elab_diag(ErrorKind::Semantic, "spice.semantic.subckt_cycle",
                                        "recursive subcircuit: " + chain, subname.loc));
        }
    }
    if (subckt_stack.size() >= 32) {
        return spice_fail(elab_diag(ErrorKind::Semantic, "spice.semantic.subckt_depth",
                                    "subcircuit nesting exceeds 32", subname.loc));
    }

    const std::vector<Token> call_nodes(card.rest.begin(),
                                        card.rest.begin() + static_cast<long>(subname_idx));
    if (call_nodes.size() != def.ports.size()) {
        return spice_fail(elab_diag(
            ErrorKind::Semantic, "spice.semantic.x_ports",
            "subcircuit " + card.name.text + " passes " +
                std::to_string(call_nodes.size()) + " nodes but '" + def.name.text +
                "' declares " + std::to_string(def.ports.size()),
            card.loc));
    }

    // Port map: lowercased local port -> caller's token (already scoped).
    std::map<std::string, Token> port_map;
    for (std::size_t i = 0; i < def.ports.size(); ++i) {
        port_map[to_lower(def.ports[i].text)] = call_nodes[i];
    }

    // Scope: subckt defaults over global; instance params (evaluated in
    // the CALLER's scope, becoming constants) override defaults (§8.4).
    ParamTable scope(&global_params_);
    for (const ParamDef& d : def.defaults) {
        scope.define(d);
    }
    for (const Token& raw : instance_params_raw) {
        const std::size_t eq = raw.text.find('=');
        if (eq == std::string::npos || eq == 0) {
            return spice_fail(elab_diag(
                ErrorKind::Parse, "spice.parse.subckt_param",
                "expected name=value in subcircuit call, got '" + raw.text + "'",
                raw.loc));
        }
        ParamDef override_def;
        override_def.name.text = raw.text.substr(0, eq);
        override_def.name.loc = raw.loc;
        WS_TRY_ASSIGN(const double value, ([&]() -> ws::core::Result<double> {
            Token t;
            t.text = raw.text.substr(eq + 1);
            t.loc = raw.loc;
            return resolve_value_token(t, caller_params);
        }()));
        override_def.expr = [] {
            ExprProgram p;
            return p;
        }();
        override_def.expr.code_ = {ExprProgram::Instr{ExprProgram::Op::Const, 0, -1}};
        override_def.expr.consts_ = {value};
        scope.define(std::move(override_def));
    }

    // The instance's own prefix chain.
    const std::string my_prefix = prefix + card.name.text + ":";
    subckt_stack.push_back(key);

    for (const Record& rec : def.body) {
        ws::core::Result<cards::ParsedCard> parsed =
            cards::parse_card(rec, true, warnings_);
        if (!parsed) {
            subckt_stack.pop_back();
            return ws::core::unexpected(std::move(parsed.error()));
        }
        const cards::ParsedCard& c = *parsed;
        if (c.kind == cards::ParsedCard::Kind::Device) {
            if (c.letter == 'X') {
                // Nested call: its node tokens are names in THIS subckt's
                // scope - map ports to the caller's tokens, prefix this
                // instance's internals - then recurse. (The subname and
                // any params: k=v tail pass through untouched; the
                // boundary is the first params:/k=v token, or the last
                // token when there is none.)
                DeviceCard nested = c.device;
                std::size_t boundary = nested.rest.size();
                for (std::size_t i = 0; i < nested.rest.size(); ++i) {
                    const std::string t = to_lower(nested.rest[i].text);
                    if (t == "params:" || nested.rest[i].text.find('=') != std::string::npos) {
                        boundary = i;
                        break;
                    }
                }
                const std::size_t node_end =
                    (boundary == 0) ? 0 : boundary - 1;  // subname sits at boundary-1
                for (std::size_t i = 0; i < node_end && i < nested.rest.size(); ++i) {
                    if (const auto port = port_map.find(to_lower(nested.rest[i].text));
                        port != port_map.end()) {
                        nested.rest[i] = port->second;
                    } else {
                        nested.rest[i].text = my_prefix + nested.rest[i].text;
                    }
                }
                ws::core::Status st = flatten_x(nested, scope, my_prefix, subckt_stack);
                if (!st) {
                    subckt_stack.pop_back();
                    return st;
                }
                continue;
            }
            // Rewrite name, nodes and control names into scoped tokens.
            DeviceCard scoped = c.device;
            scoped.name.text = my_prefix + c.device.name.text;
            const auto shapes = device_shapes();
            const auto shape_it = shapes.find(c.letter);
            const DeviceShape shape = (shape_it != shapes.end()) ? shape_it->second
                                                                 : DeviceShape{};
            for (std::size_t i = 0; i < shape.nodes && i < scoped.rest.size(); ++i) {
                const Token& local = scoped.rest[i];
                if (const auto port = port_map.find(to_lower(local.text));
                    port != port_map.end()) {
                    scoped.rest[i] = port->second;
                } else {
                    scoped.rest[i].text = my_prefix + local.text;  // internal node
                }
            }
            if (shape.control_token >= 0 &&
                static_cast<std::size_t>(shape.control_token) < scoped.rest.size()) {
                scoped.rest[static_cast<std::size_t>(shape.control_token)].text =
                    my_prefix +
                    scoped.rest[static_cast<std::size_t>(shape.control_token)].text;
            }
            ws::core::Status st = add_device_card(scoped, c.letter, scope);
            if (!st) {
                subckt_stack.pop_back();
                return st;
            }
        }
        // .model records in the body: validated by parse_card already;
        // device models land with M5 (resistor models parse, none consume
        // them yet).
    }
    subckt_stack.pop_back();
    return {};
}

ws::core::Status Elaborator::resolve_controls() {
    for (const ControlRequest& req : pending_controls_) {
        const std::string key = to_lower(req.control.text);
        const auto it = vsource_branches_.find(key);
        if (it == vsource_branches_.end()) {
            return spice_fail(elab_diag(
                ErrorKind::Semantic, "spice.semantic.control_source",
                "controlling source '" + req.control.text +
                    "' is not a known voltage source (v1 controls name V sources)",
                req.control.loc));
        }
        req.device->set_control_branch(it->second);
    }
    return {};
}

ws::core::Status Elaborator::check_topology() {
    const std::size_t n = circuit_.node_count();

    // --- voltage-constraint loops (V, E/H outputs) ------------------------
    {
        // Ground joins as virtual node n: constraint edges to ground (the
        // common parallel-sources-to-ground loop) participate like any
        // other edge.
        const std::size_t ground = n;
        std::vector<std::vector<std::pair<std::size_t, std::size_t>>> adj(
            n + 1);  // node -> (neighbor, device index)
        for (std::size_t di = 0; di < circuit_.devices.size(); ++di) {
            for (const auto& [a, b] : circuit_.devices[di]->constraint_edges()) {
                const std::size_t ea = (a == kGround) ? ground : a;
                const std::size_t eb = (b == kGround) ? ground : b;
                if (ea == eb) {
                    continue;  // degenerate self-constraint (e.g. V x x)
                }
                adj[ea].emplace_back(eb, di);
                adj[eb].emplace_back(ea, di);
            }
        }
        std::vector<bool> seen(n + 1, false);
        for (std::size_t start = 0; start <= n; ++start) {
            if (seen[start]) {
                continue;
            }
            // BFS over the constraint graph; a non-tree edge closes a loop.
            std::vector<std::size_t> parent(n + 1, static_cast<std::size_t>(-1));
            std::vector<std::size_t> parent_edge(n + 1, static_cast<std::size_t>(-1));
            std::queue<std::size_t> q;
            seen[start] = true;
            q.push(start);
            while (!q.empty()) {
                const std::size_t u = q.front();
                q.pop();
                for (const auto& [v, di] : adj[u]) {
                    if (!seen[v]) {
                        seen[v] = true;
                        parent[v] = u;
                        parent_edge[v] = di;
                        q.push(v);
                    } else if (parent_edge[u] != di) {
                        // Non-tree edge: cycle. (Compare the EDGE, not the
                        // node: a second parallel edge to the parent is a
                        // loop too.) Walk u->root and v->root to the
                        // LCA, collecting devices.
                        std::vector<std::size_t> cycle;
                        auto path_to_root = [&](std::size_t x) {
                            std::vector<std::size_t> path;
                            while (x != static_cast<std::size_t>(-1)) {
                                path.push_back(x);
                                x = (parent[x] == static_cast<std::size_t>(-1))
                                        ? static_cast<std::size_t>(-1)
                                        : parent[x];
                            }
                            return path;
                        };
                        const auto pu = path_to_root(u);
                        const auto pv = path_to_root(v);
                        std::set<std::size_t> in_pv;
                        for (const std::size_t x : pv) {
                            in_pv.insert(x);
                        }
                        std::size_t lca = static_cast<std::size_t>(-1);
                        for (const std::size_t x : pu) {
                            if (in_pv.count(x) != 0) {
                                lca = x;
                                break;
                            }
                        }
                        std::vector<std::size_t> loop_nodes;
                        for (const std::size_t x : pu) {
                            if (x == lca) {
                                break;
                            }
                            loop_nodes.push_back(x);
                        }
                        for (std::size_t i = 0; i < pv.size() && pv[i] != lca; ++i) {
                            loop_nodes.push_back(pv[i]);
                        }
                        for (const std::size_t x : loop_nodes) {
                            if (parent_edge[x] != static_cast<std::size_t>(-1)) {
                                cycle.push_back(parent_edge[x]);
                            }
                        }
                        cycle.push_back(di);
                        std::set<std::size_t> unique(cycle.begin(), cycle.end());
                        std::vector<std::string> names;
                        for (const std::size_t c : unique) {
                            names.push_back(circuit_.devices[c]->name());
                        }
                        Diagnostic d = elab_diag(
                            ErrorKind::Topology, "spice.topology.vsource_loop",
                            "voltage-source loop", SourceLoc{});
                        d.names = std::move(names);
                        d.hint = "add series resistance in one branch of the loop";
                        return spice_fail(std::move(d));
                    }
                }
            }
        }
    }

    // --- no DC path to ground (§4.4/§6.6) ----------------------------------
    {
        // Union-find over nodes; ground joins as its own set via edges that
        // touch it. Conductance paths (R) and constraint paths (V/E/H
        // outputs) connect; current-source outputs and control inputs do
        // not.
        const std::size_t ground = n;  // virtual ground slot
        std::vector<std::size_t> parent(n + 1);
        for (std::size_t i = 0; i <= n; ++i) {
            parent[i] = i;
        }
        auto find = [&](std::size_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        auto unite = [&](std::size_t a, std::size_t b) {
            a = find(a);
            b = find(b);
            if (a != b) {
                parent[a] = b;
            }
        };
        for (const auto& dev : circuit_.devices) {
            for (const auto& [a, b] : dev->conductance_edges()) {
                const std::size_t ea = (a == kGround) ? ground : a;
                const std::size_t eb = (b == kGround) ? ground : b;
                unite(ea, eb);
            }
            for (const auto& [a, b] : dev->constraint_edges()) {
                const std::size_t ea = (a == kGround) ? ground : a;
                const std::size_t eb = (b == kGround) ? ground : b;
                unite(ea, eb);
            }
        }
        const std::size_t ground_root = find(ground);
        for (std::size_t i = 0; i < n; ++i) {
            if (find(i) != ground_root) {
                Diagnostic d = elab_diag(
                    ErrorKind::Topology, "spice.topology.no_dc_path",
                    "no DC path to ground from node '" + circuit_.node_names[i] + "'",
                    SourceLoc{});
                d.hint =
                    "common causes: floating node, series capacitors (M2), open switch";
                return spice_fail(std::move(d));
            }
        }
    }
    return {};
}

ws::core::Status Elaborator::elaborate() {
    // Global parameter scope. Every definition is resolved eagerly even
    // when nothing references it: an unreferenced cycle (a=b+1, b=a) is
    // still an error naming the cycle (§8.4), not a silent pass.
    for (const ParamDef& def : ast_.params) {
        global_params_.define(def);
    }
    for (const ParamDef& def : ast_.params) {
        WS_TRY(global_params_.value(to_lower(def.name.text)));
    }

    // Devices in deck order; X cards flatten recursively. The kind letter
    // is the FIRST character of the (unscoped) name - scoped instances
    // ("X1:Rt") carry their letter separately so the prefix never wins.
    std::vector<std::string> subckt_stack;
    for (const DeviceCard& card : ast_.devices) {
        const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(
            card.name.text.empty() ? ' ' : card.name.text[0])));
        if (letter == 'X') {
            WS_TRY(flatten_x(card, global_params_, "", subckt_stack));
        } else {
            WS_TRY(add_device_card(card, letter, global_params_));
        }
    }

    WS_TRY(resolve_controls());
    return check_topology();
}

// --- public entry ----------------------------------------------------------------

ws::core::Result<Circuit> elaborate_deck(const DeckAst& ast,
                                         std::vector<Warning>& warnings) {
    Elaborator elab(ast, warnings);
    WS_TRY(elab.elaborate());
    return elab.take_circuit();
}

}  // namespace ws::spice
