// Circuit finalization (the arena layout of §3.5 rev): flattens every
// device's param_specs() into one table, records per-device spans, and
// groups device indices by kind letter for future batched stamping.
// Elaboration (elaborate.cpp) and the CircuitBuilder both end here.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <ws/spice/circuit.hpp>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/run.hpp>

namespace ws::spice {

ws::core::Status finalize_circuit(Circuit& c) {
    c.param_span.assign(c.devices.size(), {});
    c.state_span.assign(c.devices.size(), {});
    c.param_table.clear();
    c.param_total = 0;
    c.state_total = 0;
    c.by_kind.clear();

    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        const Device& dev = *c.devices[i];
        std::vector<ParamSpec> specs = dev.param_specs();
        c.param_span[i] = {c.param_total, specs.size()};
        for (ParamSpec& s : specs) {
            c.param_table.push_back(std::move(s));
        }
        c.param_total += specs.size();
        c.state_span[i] = {c.state_total, dev.state_slots()};
        c.state_total += dev.state_slots();
        c.by_kind[dev.kind()].push_back(i);
    }
    // Cross-device links (K-coupled inductors) resolve against the
    // finished layout; a partner lost between coupling and finalize is a
    // programmer error, not a deck error.
    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        WS_TRY(c.devices[i]->link_dynamic(c, i));
    }
    return {};
}

// --- .options ------------------------------------------------------------------
//
// Moved from elaborate.cpp unchanged in behavior (the elaborator no
// longer owns it; RunSettings is where it belongs now that run_deck
// binds parameter scopes itself).

namespace {

Diagnostic opt_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

}  // namespace

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
            } else if (lower == "itl4") {
                out.itl4 = static_cast<int>(v);
            } else if (lower == "chgtol") {
                out.chgtol = v;
            } else if (lower == "trtol") {
                out.trtol = v;
            } else if (lower == "temp") {
                out.temp = v;
            } else if (lower == "tnom") {
                // Accepted and recorded; no M1/M2 device reads it yet
                // (temp-dependent models land M5).
                out.tnom = v;
            } else if (lower == "seed") {
                out.seed = static_cast<std::uint64_t>(v);
            } else if (lower == "compat") {
                const std::string mode = to_lower(value.text);
                if (mode != "ngspice" && mode != "ltspice" && mode != "strict") {
                    return spice_fail(opt_diag(
                        ErrorKind::Parse, "spice.parse.compat",
                        "'.options compat=" + value.text +
                            "': expected ngspice, ltspice or strict", card.loc));
                }
                out.compat = mode;
            } else if (lower == "solver") {
                const std::string backend = to_lower(value.text);
                if (backend != "eigen") {
                    return spice_fail(opt_diag(
                        ErrorKind::Parse, "spice.parse.solver",
                        "'.options solver=" + value.text +
                            "': only the Eigen backend exists until KLU lands (M0b)",
                        card.loc));
                }
                out.solver = backend;
            } else if (lower == "gshunt") {
                return spice_fail(opt_diag(
                    ErrorKind::Parse, "spice.parse.unsupported_card",
                    "'.options gshunt' lands with the convergence ladder (M3)",
                    card.loc));
            }
        }
    }
    return {};
}

}  // namespace ws::spice
