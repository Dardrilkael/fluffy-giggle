#pragma once

#include <cstdint>
#include <string>
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

// One finding. `where` names the object it points at, pre-rendered by the
// producer ("R3", "R3 pin 2", "net N004", "directives", "" = file-level);
// `code` is the stable machine name ("circsim.erc.floating_pin").
struct Diag {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::string where;

    // The render the CLI and the Problems panel show:
    //   "<where>: error: <message>"  /  "error: <message>" (file-level)
    [[nodiscard]] std::string render() const;
};

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
