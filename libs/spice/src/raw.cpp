// The .raw writer (§7.8 rev): columnar Table -> the LTspice/ngspice
// binary waveform format. The header is plain text through "Binary:";
// the payload is native-endian doubles, one row per point (axis values
// first, then signals; complex columns are re/im pairs for .ac tables).

#include <cstring>
#include <string>
#include <vector>

#include <ws/spice/raw.hpp>

namespace ws::spice {

namespace {

const char* plot_name(AnalysisKind k) {
    switch (k) {
        case AnalysisKind::Tran: return "Transient Analysis";
        case AnalysisKind::Dc: return "DC Analysis";
        case AnalysisKind::Ac: return "AC Analysis";
        case AnalysisKind::Op: return "Operating Point";
    }
    return "Analysis";
}

// The .raw variable type of a column, by name.
std::string var_type(std::string_view name, std::size_t axis) {
    if (axis == 0) {
        if (name == "time") {
            return "time";
        }
        if (name == "frequency") {
            return "frequency";
        }
        return "v-sweep";
    }
    if (name.rfind("i(", 0) == 0) {
        return "current";
    }
    return "voltage";
}

// Unwraps re(x)/im(x) pair signals of an .ac table into (name, re-signal,
// im-signal) triples; empty for non-AC tables (whose signals stay real).
struct ComplexVar {
    std::string name;
    const std::vector<double>* re;
    const std::vector<double>* im;
};

std::vector<ComplexVar> ac_complex_vars(const Table& t) {
    std::vector<ComplexVar> out;
    if (t.analysis != AnalysisKind::Ac) {
        return out;
    }
    for (std::size_t i = 0; i + 1 < t.signals.size(); ++i) {
        const Signal& a = t.signals[i];
        const Signal& b = t.signals[i + 1];
        const bool re_first = a.name.rfind("re(", 0) == 0 && a.name.back() == ')' &&
                              b.name.rfind("im(", 0) == 0 && b.name.back() == ')' &&
                              a.name.substr(3) == b.name.substr(3);
        if (!re_first) {
            continue;
        }
        ComplexVar v;
        v.name = a.name.substr(3, a.name.size() - 4);
        v.re = &a.data;
        v.im = &b.data;
        out.push_back(std::move(v));
        ++i;  // consume the pair
    }
    return out;
}

void append_double(std::string& out, double v) {
    char bytes[sizeof(double)];
    std::memcpy(bytes, &v, sizeof(double));
    out.append(bytes, sizeof(double));
}

}  // namespace

std::string table_raw(const Table& t, const RawOptions& opts) {
    const std::vector<ComplexVar> complex_vars = ac_complex_vars(t);
    const bool is_complex = !complex_vars.empty();

    // Variable list: sweep axes, then signals (complex pairs merged).
    std::vector<std::pair<std::string, std::string>> vars;  // (name, type)
    for (std::size_t k = 0; k < t.sweep_names.size(); ++k) {
        vars.emplace_back(t.sweep_names[k], var_type(t.sweep_names[k], k));
    }
    if (is_complex) {
        for (const ComplexVar& v : complex_vars) {
            vars.emplace_back(v.name, var_type(v.name, 1));
        }
    } else {
        for (const Signal& s : t.signals) {
            vars.emplace_back(s.name, var_type(s.name, 1));
        }
    }

    const std::size_t points = t.row_count();
    std::string out;
    out.reserve(512 + points * vars.size() * sizeof(double));
    out += "Title: * " + opts.title + "\n";
    if (!opts.date.empty()) {
        out += "Date: " + opts.date + "\n";
    }
    out += std::string("Plotname: ") + plot_name(t.analysis) + "\n";
    out += is_complex ? "Flags: complex\n" : "Flags: real\n";
    out += "No. Variables: " + std::to_string(vars.size()) + "\n";
    out += "No. Points: " + std::to_string(points) + "\n";
    if (!opts.command.empty()) {
        out += "Command: " + opts.command + "\n";
    }
    out += "Variables:\n";
    for (std::size_t i = 0; i < vars.size(); ++i) {
        out += "\t" + std::to_string(i) + "\t" + vars[i].first + "\t" +
               vars[i].second + "\n";
    }
    out += "Binary:\n";

    // Payload: point rows of axis values then signal values.
    for (std::size_t p = 0; p < points; ++p) {
        for (const auto& ax : t.sweep_axes) {
            append_double(out, p < ax.size() ? ax[p] : 0.0);
        }
        if (is_complex) {
            for (const ComplexVar& v : complex_vars) {
                append_double(out, p < v.re->size() ? (*v.re)[p] : 0.0);
                append_double(out, p < v.im->size() ? (*v.im)[p] : 0.0);
            }
        } else {
            for (const Signal& s : t.signals) {
                append_double(out, p < s.data.size() ? s.data[p] : 0.0);
            }
        }
    }
    return out;
}

std::string results_raw(const Results& r, const RawOptions& opts) {
    for (const Table& t : r.tables) {
        if (t.analysis != AnalysisKind::Op || r.tables.size() == 1) {
            return table_raw(t, opts);
        }
    }
    return r.tables.empty() ? std::string() : table_raw(r.tables.front(), opts);
}

}  // namespace ws::spice
