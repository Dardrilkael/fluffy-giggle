// apps/spice - the ws::spice CLI (DESIGN.md §0.6, §9.5): a thin
// parse -> simulate -> write shell. argv, file I/O and exit codes only;
// every real behavior lives in the lib.
//
//   spice run <deck.cir> [-o out.csv] [-I <include-dir>]...
//
// Exit codes: 0 success; 1 simulation or deck error; 2 usage or file-I/O
// error (§9.5). Warnings print to stderr (§9.3), never fail the run.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>

namespace {

namespace fs = std::filesystem;

int usage() {
    std::fprintf(stderr,
                 "usage: spice run <deck.cir> [-o out.csv] [-I <include-dir>]...\n"
                 "                  [--keep-going]\n"
                 "  --keep-going   run the remaining .step points after a failure\n"
                 "                 and report the list at the end (§7.6)\n");
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
void print_warnings(const std::vector<ws::spice::Warning>& warnings,
                    std::size_t& printed) {
    for (std::size_t i = printed; i < warnings.size(); ++i) {
        std::fprintf(stderr, "%s\n", warnings[i].render().c_str());
    }
    printed = warnings.size();
}

int cmd_run(const std::vector<std::string>& args) {
    // spice run <deck.cir> [-o out.csv] [-I <dir>]...
    if (args.empty()) {
        return usage();
    }
    const fs::path deck_path = args[0];
    std::string out_path;  // empty = stdout
    std::vector<std::string> include_dirs;
    bool keep_going = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: -o needs a file name\n");
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
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", args[i].c_str());
            return usage();
        }
    }

    const ws::core::Result<std::string> deck_text = read_file(deck_path);
    if (!deck_text) {
        return io_error(deck_text.error().message);
    }

    ws::spice::ParseOptions parse_opts;
    parse_opts.include_dirs = include_dirs;
    parse_opts.include_resolver =
        disk_resolver(include_dirs, deck_path.has_parent_path()
                                       ? deck_path.parent_path()
                                       : fs::path("."));

    std::vector<ws::spice::Warning> warnings;
    std::size_t warnings_printed = 0;

    ws::core::Result<ws::spice::Deck> deck =
        ws::spice::read_deck(deck_path.string(), *deck_text, parse_opts);
    if (!deck) {
        std::fprintf(stderr, "%s\n", deck.error().message.c_str());
        return 1;
    }

    ws::core::Result<ws::spice::DeckAst> ast = ws::spice::build_deck_ast(*deck, warnings);
    print_warnings(warnings, warnings_printed);
    if (!ast) {
        std::fprintf(stderr, "%s\n", ast.error().message.c_str());
        return 1;
    }

    ws::spice::RunSettings settings;
    settings.keep_going = keep_going;  // §7.6: run the remaining step points
    const ws::core::Result<ws::spice::RunOutput> run =
        ws::spice::run_deck(*ast, settings, warnings);
    print_warnings(warnings, warnings_printed);
    if (!run) {
        std::fprintf(stderr, "%s\n", run.error().message.c_str());
        return 1;
    }

    const std::string csv = ws::spice::results_csv(run->results);
    if (out_path.empty()) {
        std::fputs(csv.c_str(), stdout);
    } else {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            return io_error("cannot write '" + out_path + "'");
        }
        out << csv;
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
    std::fprintf(stderr, "error: unknown command '%s'\n", args[0].c_str());
    return usage();
}
