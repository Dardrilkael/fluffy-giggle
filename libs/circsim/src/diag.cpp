#include <ws/circsim/diag.hpp>

namespace ws::circsim {

std::string Diag::render() const {
    std::string out;
    if (!where.empty()) {
        out += where;
        out += ": ";
    }
    switch (severity) {
        case Severity::Note: out += "note: "; break;
        case Severity::Warning: out += "warning: "; break;
        case Severity::Error: out += "error: "; break;
    }
    out += message;
    return out;
}

}  // namespace ws::circsim
