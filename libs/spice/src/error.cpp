#include <ws/spice/error.hpp>

#include <utility>

namespace ws::spice {
namespace {

// The §9.2 shape; `severity` is "error" or "warning".
std::string render_diagnostic(const Diagnostic& d, const char* severity) {
    std::string out;
    if (d.loc.valid) {
        out += d.loc.file;
        out += ':' + std::to_string(d.loc.line);
        out += ':' + std::to_string(d.loc.col);
        out += std::string(": ") + severity + ": ";
    } else {
        out = std::string(severity) + ": ";
        if (!d.analysis.empty()) {
            out += '[' + d.analysis + "] ";
        }
    }
    out += d.message;
    if (!d.names.empty()) {
        out += ": ";
        for (std::size_t i = 0; i < d.names.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += d.names[i];
        }
    }
    if (!d.hint.empty()) {
        out += "\n  hint: ";
        out += d.hint;
    }
    return out;
}

}  // namespace

std::string Diagnostic::render() const {
    return render_diagnostic(*this, "error");
}

std::string Warning::render() const {
    return render_diagnostic(diag, "warning");
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
