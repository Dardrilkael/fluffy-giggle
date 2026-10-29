// apps/spice - the ws::spice CLI (skeleton; grows with M1).
// `spice run circuit.cir -o out.csv`. argv and file I/O only; the
// simulator itself never lives here.

#include <cstdio>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: spice run <deck.cir> [-o out.csv] [-I <include-dir>]...\n");
    return 2;  // §9.5: usage errors exit 2
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    std::fprintf(stderr, "spice: '%s' not implemented yet (M1 in flight)\n",
                 args[0].c_str());
    return 1;
}
