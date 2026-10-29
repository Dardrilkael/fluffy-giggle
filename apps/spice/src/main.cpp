// apps/spice - the ws::spice CLI (DESIGN.md §0.6, §9.5): a thin
// parse -> simulate -> write shell. argv, file I/O and exit codes only;
// every real behavior lives in the lib.
//
//   spice run <deck.cir> [-o out|-] [--format csv|raw] [-I <dir>]...
//                       [--keep-going] [--strict-meas] [--werror]
//   spice check <deck.cir> [-I <dir>]... [--werror]
//
// Exit codes: 0 success; 1 simulation or deck error (or a warning under
// --werror); 2 usage or file-I/O error (§9.5). Warnings print to stderr
// and never fail a run (§9.3).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/lint.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/raw.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>

namespace {

namespace fs = std::filesystem;

int usage() {
    std::fprintf(stderr,
                 "usage: spice run <deck.cir> [-o out.csv|-] [--format csv|raw]"
                 " [-I <include-dir>]...\n"
                 "                  [--keep-going] [--strict-meas] [--werror]\n"
                 "       spice check <deck.cir> [-I <include-dir>]... [--werror]\n"
                 "  -o -           write to stdout (piping; binary for --format raw)\n"
                 "  --format raw   write the LTspice/ngspice .raw waveform format\n"
                 "                 instead of CSV (the run's primary table)\n"
                 "  --keep-going   run the remaining .step points after a failure\n"
                 "                 and report the list at the end (§7.6)\n"
                 "  --strict-meas  exit 1 when any .meas fails (§9.5)\n"
                 "  --werror       promote warnings to errors (§9.3)\n"
                 "  check          lint without simulating: topology checks, unused\n"
                 "                 models, the M/milli and F/femto number traps\n");
    return 2;  // §9.5: usage errors exit 2
}

int io_error(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;  // §9.5: file-I/O errors exit 2
}

ws::core::Result<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ws::core::Error e = ws::core::make_error(
            static_cast<std::int32_t>(ws::spice::ErrorKind::Io), "spice",
            "cannot open '" + path.string() + "'");
        return ws::core::unexpected(std::move(e));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The disk seam for '.include' (§8.1): the including file's own directory
// first, then the -I paths in order.
ws::spice::IncludeResolver disk_resolver(const std::vector<std::string>& include_dirs,
                                         const fs::path& deck_dir) {
    return [include_dirs, deck_dir](const std::string& path) {
        fs::path candidate(path);
        if (candidate.is_absolute()) {
            return read_file(candidate);
        }
        if (ws::core::Result<std::string> text = read_file(deck_dir / candidate)) {
            return text;
        }
        for (const std::string& dir : include_dirs) {
            if (ws::core::Result<std::string> text = read_file(fs::path(dir) / candidate)) {
                return text;
            }
        }
        ws::core::Error e = ws::core::make_error(
            static_cast<std::int32_t>(ws::spice::ErrorKind::Io), "spice",
            "cannot resolve include '" + path + "'");
        return ws::core::Result<std::string>(ws::core::unexpected(std::move(e)));
    };
}

// Warnings accumulate across stages (parse -> ast -> run); each stage's
// print call emits only what it added, so nothing is printed twice.
// --werror flips the run's exit code when any landed (§9.3).
int print_warnings(const std::vector<ws::spice::Warning>& warnings,
                   std::size_t& printed, bool werror) {
    for (std::size_t i = printed; i < warnings.size(); ++i) {
        std::fprintf(stderr, "%s\n", warnings[i].render().c_str());
    }
    const bool any_new = warnings.size() > printed;
    printed = warnings.size();
    return (any_new && werror) ? 1 : 0;
}

// parse -> ast for one deck; the shared front of run/check.
ws::core::Result<ws::spice::DeckAst> load_ast(const fs::path& deck_path,
                                              const std::vector<std::string>& include_dirs,
                                              std::vector<ws::spice::Warning>& warnings,
                                              std::size_t& printed, int& exit_code,
                                              bool werror) {
    const ws::core::Result<std::string> deck_text = read_file(deck_path);
    if (!deck_text) {
        exit_code = io_error(deck_text.error().message);
        return ws::core::unexpected(deck_text.error());
    }
    ws::spice::ParseOptions parse_opts;
    parse_opts.include_dirs = include_dirs;
    parse_opts.include_resolver =
        disk_resolver(include_dirs, deck_path.has_parent_path()
                                       ? deck_path.parent_path()
                                       : fs::path("."));
    const ws::core::Result<ws::spice::Deck> deck =
        ws::spice::read_deck(deck_path.string(), *deck_text, parse_opts);
    if (!deck) {
        std::fprintf(stderr, "%s\n", deck.error().message.c_str());
        exit_code = 1;
        return ws::core::unexpected(deck.error());
    }
    const ws::core::Result<ws::spice::DeckAst> ast = ws::spice::build_deck_ast(*deck, warnings);
    if (const int w = print_warnings(warnings, printed, werror); w != 0 && exit_code == 0) {
        exit_code = w;
    }
    if (!ast) {
        std::fprintf(stderr, "%s\n", ast.error().message.c_str());
        exit_code = (exit_code == 0) ? 1 : exit_code;
        return ws::core::unexpected(ast.error());
    }
    return ast;
}

int cmd_run(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage();
    }
    const fs::path deck_path = args[0];
    std::string out_path;  // empty = stdout
    std::vector<std::string> include_dirs;
    bool keep_going = false;
    bool strict_meas = false;
    bool werror = false;
    bool raw = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -o needs a file name (or '-')\n");
                return usage();
            }
            out_path = args[++i];
        } else if (args[i] == "-I") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -I needs a directory\n");
                return usage();
            }
            include_dirs.push_back(args[++i]);
        } else if (args[i] == "--keep-going") {
            keep_going = true;
        } else if (args[i] == "--strict-meas") {
            strict_meas = true;
        } else if (args[i] == "--werror") {
            werror = true;
        } else if (args[i] == "--format") {
            if (i + 1 >= args.size() || (args[i + 1] != "csv" && args[i + 1] != "raw")) {
                std::fprintf(stderr, "error: --format takes csv or raw\n");
                return usage();
            }
            raw = (args[++i] == "raw");
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", args[i].c_str());
            return usage();
        }
    }

    std::vector<ws::spice::Warning> warnings;
    std::size_t printed = 0;
    int exit_code = 0;
    const auto ast =
        load_ast(deck_path, include_dirs, warnings, printed, exit_code, werror);
    if (!ast) {
        return exit_code != 0 ? exit_code : 1;
    }

    ws::spice::RunSettings settings;
    settings.keep_going = keep_going;  // §7.6: run the remaining step points
    const ws::core::Result<ws::spice::RunOutput> run =
        ws::spice::run_deck(*ast, settings, warnings);
    if (const int w = print_warnings(warnings, printed, werror); w != 0 &&
        exit_code == 0) {
        exit_code = w;
    }
    if (!run) {
        std::fprintf(stderr, "%s\n", run.error().message.c_str());
        return 1;
    }

    // .meas results print to stdout after the CSV (ngspice's shape);
    // a failed measurement is a fact about the run, not an error (§9.5),
    // unless --strict-meas promotes it.
    bool meas_failed = false;
    for (const ws::spice::Measurement& m : run->measurements) {
        if (m.value.has_value()) {
            std::fprintf(stdout, "%s: %.12g\n", m.name.c_str(), *m.value);
        } else {
            std::fprintf(stdout, "%s: FAILED (%s)\n", m.name.c_str(), m.note.c_str());
            meas_failed = true;
        }
    }

    const std::string body = raw ? ws::spice::results_raw(run->results)
                                 : ws::spice::results_csv(run->results);
    if (out_path.empty() || out_path == "-") {
        // stdout: the documented pipe target (-o -; the default too).
        std::fwrite(body.data(), 1, body.size(), stdout);
    } else {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            return io_error("cannot write '" + out_path + "'");
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    if (meas_failed && strict_meas) {
        return 1;  // §9.5: --strict-meas promotes .meas failures to exit 1
    }
    return exit_code;
}

int cmd_check(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage();
    }
    const fs::path deck_path = args[0];
    std::vector<std::string> include_dirs;
    bool werror = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-I") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -I needs a directory\n");
                return usage();
            }
            include_dirs.push_back(args[++i]);
        } else if (args[i] == "--werror") {
            werror = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", args[i].c_str());
            return usage();
        }
    }

    std::vector<ws::spice::Warning> warnings;
    std::size_t printed = 0;
    int exit_code = 0;
    const auto ast =
        load_ast(deck_path, include_dirs, warnings, printed, exit_code, werror);
    if (!ast) {
        return exit_code != 0 ? exit_code : 1;
    }

    // The lint pass: number traps + full elaboration (topology checks,
    // unused models). Nothing simulates.
    const ws::core::Status st = ws::spice::lint_deck(*ast, warnings);
    if (const int w = print_warnings(warnings, printed, werror); w != 0 &&
        exit_code == 0) {
        exit_code = w;
    }
    if (!st) {
        std::fprintf(stderr, "%s\n", st.error().message.c_str());
        return 1;
    }
    return exit_code;
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
    if (args[0] == "check") {
        return cmd_check(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    std::fprintf(stderr, "error: unknown command '%s'\n", args[0].c_str());
    return usage();
}
