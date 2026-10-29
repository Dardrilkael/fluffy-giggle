// The netlist writer (§10.2 rev): Circuit + RunState -> flattened deck.
// Node names come from the circuit's table; values from the state's bound
// parameters; models from the circuit's used_models. Every device must
// write a card or the writer fails naming it - never a silent gap.

#include <charconv>
#include <map>
#include <string>
#include <vector>

#include <ws/spice/netlist.hpp>

namespace ws::spice {

namespace {

// The concrete CardWriter: names and bound values out of (Circuit,
// RunState).
class StateCardWriter final : public Device::CardWriter {
public:
    StateCardWriter(const Circuit& c, const RunState& rs) : c_(c), rs_(rs) {
        for (const auto& di : c_.branch_devices) {
            const Device& dev = *c_.devices[di];
            branch_owner_[dev.branch()] = dev.name();
        }
    }

    [[nodiscard]] const std::string& node(NodeId id) const override {
        static const std::string ground = "0";
        if (id == kGround) {
            return ground;
        }
        return c_.node_names[id];
    }

    [[nodiscard]] const std::string& branch_owner(BranchId br) const override {
        const auto it = branch_owner_.find(br);
        if (it == branch_owner_.end()) {
            static const std::string missing = "?";
            return missing;
        }
        return it->second;
    }

    [[nodiscard]] const double* params(const Device& dev) const override {
        for (std::size_t i = 0; i < c_.devices.size(); ++i) {
            if (c_.devices[i].get() == &dev) {
                return rs_.params_.data() + c_.param_span[i].begin;
            }
        }
        return nullptr;
    }

private:
    const Circuit& c_;
    const RunState& rs_;
    std::map<BranchId, std::string> branch_owner_;
};

Diagnostic netlist_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

// One .model card from its definition. `params` holds the raw card tokens
// (glued "key=value" spellings, paren glue at both ends); re-emitting
// them paren-stripped and space-joined inside the parens reproduces a
// card the parser reads back identically.
void write_model(std::string& out, const ModelDef& m) {
    out += ".model " + m.name.text + " " + m.type + "(";
    bool first = true;
    for (std::size_t i = 0; i < m.params.size(); ++i) {
        std::string text = m.params[i].text;
        while (!text.empty() && text.front() == '(') {
            text.erase(text.begin());
        }
        while (!text.empty() && text.back() == ')') {
            text.pop_back();
        }
        if (text.empty()) {
            continue;
        }
        if (!first) {
            out += " ";
        }
        out += text;
        first = false;
    }
    out += ")\n";
}

}  // namespace

std::string Device::CardWriter::num(double v) {
    // Shortest spelling that round-trips exactly (to_chars): the written
    // deck re-parses to bit-identical values.
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec != std::errc{}) {
        return "0";
    }
    return std::string(buf, res.ptr);
}

ws::core::Result<std::string> write_netlist(const Circuit& c, const RunState& rs,
                                            const NetlistOptions& opts) {
    StateCardWriter w(c, rs);
    std::string out;
    out += opts.title + "\n";
    if (opts.comment_temp) {
        out += "* flattened by ws::spice; temp = " + Device::CardWriter::num(c.temp) + " C\n";
    }
    for (const auto& dev : c.devices) {
        if (!dev->write_card(out, w)) {
            Diagnostic d = netlist_diag(
                ErrorKind::Usage, "spice.usage.unwritable_device",
                "device '" + dev->name() + "' (kind '" + dev->kind() +
                    "') cannot be written to a netlist yet");
            d.names.push_back(dev->name());
            return spice_fail(std::move(d));
        }
    }
    for (const ModelDef& m : c.used_models) {
        write_model(out, m);
    }
    out += ".end\n";
    return out;
}

}  // namespace ws::spice
