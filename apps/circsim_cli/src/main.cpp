// apps/circsim_cli - the ws::circsim headless front door. argv, file I/O
// and exit codes only (the ws::spice CLI's shape); every real behavior
// lives in the lib.
//
//   circsim_cli run <file.csim> [-o out.csv|-] [--analysis spec]...
//                        [--format csv|json] [-I <dir>]... [-S <lib.csym>]...
//   circsim_cli erc <file.csim> [-S <lib.csym>]...
//   circsim_cli netlist <file.csim> [-o out|-] [-S <lib.csym>]...
//   circsim_cli symbols [-S <lib.csym>]... [--json]
//
// -S layers .csym symbol libraries over the built-in table (later files
// win; redefinitions report a warning). `symbols` lists the effective
// table - every key, its SPICE letter and pin count; --json dumps the
// full table (pins, emit, typed params, draw primitives).
//
// Exit codes: 0 success; 1 simulation or ERC error; 2 usage or file I/O
// (the workspace convention). Warnings print to stderr and never fail a
// run.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <ws/circsim/erc.hpp>
#include <ws/circsim/export.hpp>
#include <ws/circsim/format.hpp>
#include <ws/circsim/library.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/results.hpp>

namespace {

namespace fs = std::filesystem;

int usage() {
    std::fprintf(stderr,
                 "usage: circsim_cli run <file.csim> [-o out.csv|-]"
                 " [--analysis spec]... [--format csv|json] [-I <dir>]..."
                 " [-S <lib.csym>]...\n"
                 "                  --analysis spec: 'op' | 'dc V1 0 5 0.1' |"
                 " 'tran 1u 1m [tstart [tmax]] [uic]' | 'ac dec 10 1 100k'\n"
                 "                                  (repeatable; replaces the"
                 " file's setup; default .op)\n"
                 "                  -S: layer a .csym symbol library over the"
                 " built-ins (repeatable, later wins)\n"
                 "       circsim_cli erc <file.csim> [-S <lib.csym>]...\n"
                 "       circsim_cli netlist <file.csim> [-o out|-]"
                 " [-S <lib.csym>]...\n"
                 "       circsim_cli symbols [-S <lib.csym>]... [--json]\n");
    return 2;
}

int io_error(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

ws::core::Result<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            static_cast<std::int32_t>(ws::spice::ErrorKind::Io), "circsim",
            "cannot open '" + path.string() + "'"));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Loads a .csim (printing its load diagnostics); fails to exit 2.
int load(const fs::path& path, ws::circsim::Schematic& out) {
    const auto text = read_file(path);
    if (!text) {
        return io_error(text.error().message);
    }
    ws::circsim::LoadReport report;
    const auto loaded = ws::circsim::load_csim(*text, report);
    for (const ws::circsim::Diag& d : report.diags) {
        std::fprintf(stderr, "%s\n", d.render().c_str());
    }
    if (!loaded) {
        std::fprintf(stderr, "error: %s\n", loaded.error().message.c_str());
        return 2;
    }
    out = std::move(*loaded);
    return 0;
}

// Layers every -S library (in order) onto `lib`, printing diagnostics.
// Hard failures (unreadable file, not a library) fail to exit 2.
int load_symbols(const std::vector<std::string>& paths,
                 ws::circsim::SymbolLibrary& lib) {
    for (const std::string& path : paths) {
        const auto text = read_file(fs::path(path));
        if (!text) {
            return io_error(text.error().message);
        }
        std::vector<ws::circsim::Diag> diags;
        const ws::core::Status st =
            lib.load_text(*text, path, diags);
        for (const ws::circsim::Diag& d : diags) {
            std::fprintf(stderr, "%s\n", d.render().c_str());
        }
        if (!st) {
            std::fprintf(stderr, "error: %s\n", st.error().message.c_str());
            return 2;
        }
    }
    return 0;
}

// Consumes one -S/--symbols value (the caller matched the flag);
// returns false (after printing) when the flag has no argument left.
bool take_symbols_value(const std::vector<std::string>& args, std::size_t& i,
                         std::vector<std::string>& out) {
    if (i + 1 >= args.size()) {
        std::fprintf(stderr, "error: %s needs a .csym file name\n",
                     args[i].c_str());
        return false;
    }
    out.push_back(args[++i]);
    return true;
}

int write_out(const std::string& out_path, const std::string& body) {
    if (out_path.empty() || out_path == "-") {
        std::fwrite(body.data(), 1, body.size(), stdout);
    } else {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            return io_error("cannot write '" + out_path + "'");
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    return 0;
}

// The disk seam for '.include' inside the directives block: the .csim's
// own directory first, then the -I paths in order.
ws::spice::IncludeResolver disk_resolver(const std::vector<std::string>& dirs,
                                         const fs::path& base) {
    return [dirs, base](const std::string& path) {
        fs::path candidate(path);
        if (candidate.is_absolute()) {
            return read_file(candidate);
        }
        if (ws::core::Result<std::string> text = read_file(base / candidate)) {
            return text;
        }
        for (const std::string& dir : dirs) {
            if (ws::core::Result<std::string> text =
                    read_file(fs::path(dir) / candidate)) {
                return text;
            }
        }
        return ws::core::Result<std::string>(
            ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                static_cast<std::int32_t>(ws::spice::ErrorKind::Io),
                "circsim", "cannot resolve include '" + path + "'")));
    };
}

int cmd_run(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage();
    }
    const fs::path csim_path = args[0];
    std::string out_path;
    std::string format = "csv";
    std::vector<std::string> include_dirs;
    std::vector<std::string> symbol_libs;
    std::vector<ws::circsim::AnalysisSpec> analyses;
    bool have_override = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -o needs a file name (or '-')\n");
                return usage();
            }
            out_path = args[++i];
        } else if (args[i] == "--format") {
            if (i + 1 >= args.size() ||
                (args[i + 1] != "csv" && args[i + 1] != "json")) {
                std::fprintf(stderr, "error: --format takes csv or json\n");
                return usage();
            }
            format = args[++i];
        } else if (args[i] == "-I") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -I needs a directory\n");
                return usage();
            }
            include_dirs.push_back(args[++i]);
        } else if (args[i] == "-S" || args[i] == "--symbols") {
            if (!take_symbols_value(args, i, symbol_libs)) {
                return usage();
            }
        } else if (args[i] == "--analysis") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: --analysis needs a spec\n");
                return usage();
            }
            const auto spec = ws::circsim::parse_analysis_spec(args[++i]);
            if (!spec) {
                std::fprintf(stderr, "error: %s\n",
                             spec.error().message.c_str());
                return 2;
            }
            analyses.push_back(*spec);
            have_override = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n",
                         args[i].c_str());
            return usage();
        }
    }

    ws::circsim::Schematic s;
    if (const int rc = load(csim_path, s); rc != 0) {
        return rc;
    }
    ws::circsim::SymbolLibrary lib;
    if (const int rc = load_symbols(symbol_libs, lib); rc != 0) {
        return rc;
    }

    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(
        s, have_override ? &analyses : nullptr,
        disk_resolver(include_dirs, csim_path.has_parent_path()
                                        ? csim_path.parent_path()
                                        : fs::path(".")),
        symbol_libs.empty() ? nullptr : &lib);
    for (const ws::circsim::Diag& d : out.problems) {
        std::fprintf(stderr, "%s\n", d.render().c_str());
    }
    for (const ws::spice::Warning& w : out.warnings) {
        std::fprintf(stderr, "%s\n", w.render().c_str());
    }
    if (!out.run) {
        std::fprintf(stderr, "%s\n", out.run.error().message.c_str());
        return 1;
    }

    // .meas results print to stdout ahead of the data (the spice CLI's
    // shape).
    for (const ws::spice::Measurement& m : out.run->measurements) {
        if (m.value.has_value()) {
            std::fprintf(stdout, "%s: %.12g\n", m.name.c_str(), *m.value);
        } else {
            std::fprintf(stdout, "%s: FAILED (%s)\n", m.name.c_str(),
                         m.note.c_str());
        }
    }

    const std::string body = format == "json"
                                 ? ws::circsim::results_json(*out.run)
                                 : ws::spice::results_csv(out.run->results);
    return write_out(out_path, body);
}

int cmd_erc(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage();
    }
    std::vector<std::string> symbol_libs;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-S" || args[i] == "--symbols") {
            if (!take_symbols_value(args, i, symbol_libs)) {
                return usage();
            }
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n",
                         args[i].c_str());
            return usage();
        }
    }
    ws::circsim::Schematic s;
    if (const int rc = load(fs::path(args[0]), s); rc != 0) {
        return rc;
    }
    ws::circsim::SymbolLibrary lib;
    if (const int rc = load_symbols(symbol_libs, lib); rc != 0) {
        return rc;
    }
    const std::vector<ws::circsim::Diag> diags = ws::circsim::run_erc(
        s, symbol_libs.empty() ? nullptr : &lib);
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const ws::circsim::Diag& d : diags) {
        std::fprintf(stdout, "%s\n", d.render().c_str());
        if (d.severity == ws::circsim::Severity::Error) {
            ++errors;
        } else if (d.severity == ws::circsim::Severity::Warning) {
            ++warnings;
        }
    }
    std::fprintf(stdout, "erc: %zu error%s, %zu warning%s\n", errors,
                 errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
    return errors > 0 ? 1 : 0;
}

int cmd_netlist(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage();
    }
    std::string out_path;
    std::vector<std::string> symbol_libs;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -o needs a file name (or '-')\n");
                return usage();
            }
            out_path = args[++i];
        } else if (args[i] == "-S" || args[i] == "--symbols") {
            if (!take_symbols_value(args, i, symbol_libs)) {
                return usage();
            }
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n",
                         args[i].c_str());
            return usage();
        }
    }

    ws::circsim::Schematic s;
    if (const int rc = load(fs::path(args[0]), s); rc != 0) {
        return rc;
    }
    ws::circsim::SymbolLibrary lib;
    if (const int rc = load_symbols(symbol_libs, lib); rc != 0) {
        return rc;
    }
    const ws::circsim::NetBuild nets =
        ws::circsim::build_nets(s, symbol_libs.empty() ? nullptr : &lib);
    for (const ws::circsim::Diag& d : nets.diags) {
        std::fprintf(stderr, "%s\n", d.render().c_str());
    }
    const auto deck = ws::circsim::export_deck(
        s, nets.nets, nullptr, nullptr,
        symbol_libs.empty() ? nullptr : &lib);
    if (!deck) {
        std::fprintf(stderr, "error: %s\n", deck.error().message.c_str());
        return 1;
    }
    return write_out(out_path, deck->text);
}

int cmd_symbols(const std::vector<std::string>& args) {
    std::vector<std::string> symbol_libs;
    bool json = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "-S" || args[i] == "--symbols") {
            if (!take_symbols_value(args, i, symbol_libs)) {
                return usage();
            }
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n",
                         args[i].c_str());
            return usage();
        }
    }
    ws::circsim::SymbolLibrary lib;
    if (const int rc = load_symbols(symbol_libs, lib); rc != 0) {
        return rc;
    }
    if (json) {
        return write_out("", lib.save(true));  // the full effective table
    }
    std::fprintf(stdout, "%-12s %-5s %-4s %-24s %s\n", "KEY", "SPICE",
                 "PINS", "NAME", "REF");
    for (const ws::circsim::SymbolDef& s : lib.symbols()) {
        const std::string letter =
            s.spice_letter == 0 ? std::string("-")
                                : std::string(1, s.spice_letter);
        std::fprintf(stdout, "%-12s %-5s %-4zu %-24s %s\n", s.key.c_str(),
                     letter.c_str(), s.pins.size(), s.name.c_str(),
                     s.ref_prefix.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    if (args[0] == "run") {
        return cmd_run(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "erc") {
        return cmd_erc(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "netlist") {
        return cmd_netlist(
            std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "symbols") {
        return cmd_symbols(
            std::vector<std::string>(args.begin() + 1, args.end()));
    }
    std::fprintf(stderr, "error: unknown command '%s'\n", args[0].c_str());
    return usage();
}
