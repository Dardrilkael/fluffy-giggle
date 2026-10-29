#include <ws/spice/error.hpp>

#include <utility>

namespace ws::spice {

std::string Diagnostic::render() const {
    std::string out;
    if (loc.valid) {
        out += loc.file;
        out += ':' + std::to_string(loc.line);
        out += ':' + std::to_string(loc.col);
        out += ": error: ";
    } else {
        out += "error: ";
        if (!analysis.empty()) {
            out += '[' + analysis + "] ";
        }
    }
    out += message;
    if (!names.empty()) {
        out += ": ";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += names[i];
        }
    }
    if (!hint.empty()) {
        out += "\n  hint: ";
        out += hint;
    }
    return out;
}

ws::core::Error make_error(Diagnostic d) {
    ws::core::Error e;
    e.code = static_cast<std::int32_t>(d.kind);
    e.category = "spice";
    e.message = d.render();
    return e;
}

ws::core::unexpected<ws::core::Error> spice_fail(Diagnostic d) {
    return ws::core::unexpected<ws::core::Error>(make_error(std::move(d)));
}

}  // namespace ws::spice
