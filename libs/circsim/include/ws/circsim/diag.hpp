#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ws::circsim {

// Severity ladder for every problem the model, the loader, net building,
// ERC and the spice export can report. Errors block a run; warnings and
// notes surface in the Problems panel.
enum class Severity : std::uint8_t {
    Note = 0,
    Warning = 1,
    Error = 2,
};

// The structured jump target a Diag carries alongside its display
// string (see Diag::target). Producers fill it directly at the source -
// no reverse-parsing of `where` needed. (Lives here, not locate.hpp,
// because the producers - erc/nets/export/format/library - must not
// depend on the locate seam.)
struct DiagTarget {
    enum class Kind : std::uint8_t {
        None = 0,      // unresolvable (or file-level: no object)
        Part = 1,      // .part valid; .pin is a pin index or npos
        Net = 2,       // .net is a NetList net id
        Wire = 3,      // .obj is a Wire id (the net builder's findings)
        Label = 4,     // .obj is a Label id (first of its name)
        Directives = 5,   // the directives block (no geometry)
        Analysis = 6,     // the analysis setup (no geometry)
    };
    Kind kind = Kind::None;
    std::uint32_t part = 0;  // Part id (Kind::Part)
    std::size_t pin = static_cast<std::size_t>(-1);  // pin index, or npos
    std::uint32_t net = 0;   // Net id (Kind::Net)
    std::uint32_t obj = 0;   // Wire / Label id

    [[nodiscard]] static constexpr std::size_t npos() {
        return static_cast<std::size_t>(-1);
    }
    [[nodiscard]] bool has_pin() const { return pin != npos(); }
};

// One finding. `where` names the object it points at, pre-rendered by
// the producer ("R3", "R3 pin 2", "net N004", "directives", "" =
// file-level) - the render the CLI and the Problems panel SHOW. `code`
// is the stable machine name ("circsim.erc.floating_pin"). `target` is
// the structured answer for anything that wants to SELECT or ZOOM:
// producers that know their object fill it directly; locate_diag
// consults it first and only falls back to parsing `where` (so the
// legacy string spellings keep working while producers migrate).
struct Diag {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::string where;
    DiagTarget target;

    // The render the CLI and the Problems panel show:
    //   "<where>: error: <message>"  /  "error: <message>" (file-level)
    [[nodiscard]] std::string render() const;
};

// The one diagnostic factory every producer rides (erc/nets/export/
// format/library each used to carry a private copy - make/net_warn/
// mapped_diag/load_diag/lib_diag - free to drift).
[[nodiscard]] inline Diag make_diag(Severity sev, std::string code,
                                    std::string message, std::string where,
                                    DiagTarget target = {}) {
    Diag d;
    d.severity = sev;
    d.code = std::move(code);
    d.message = std::move(message);
    d.where = std::move(where);
    d.target = target;
    return d;
}

[[nodiscard]] inline bool is_error(const Diag& d) {
    return d.severity == Severity::Error;
}
[[nodiscard]] inline bool any_errors(const std::vector<Diag>& diags) {
    for (const Diag& d : diags) {
        if (is_error(d)) {
            return true;
        }
    }
    return false;
}

}  // namespace ws::circsim
