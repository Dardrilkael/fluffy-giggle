// ws::spice test suite. One binary for the whole lib (the workspace
// pattern); cases arrive with the components they pin. Run via
// `ctest --preset linux -R spice`.
//
// This file: the error taxonomy (§9) - rendering shape, kind codes, the
// Error boundary. Number-parser and later cases grow below as the parser
// lands.

#include <string>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/spice/error.hpp>

using ws::spice::Diagnostic;
using ws::spice::ErrorKind;
using ws::spice::SourceLoc;

TEST_CASE("diagnostics render the section 9.2 message shapes") {
    SUBCASE("with a source location") {
        Diagnostic d;
        d.kind = ErrorKind::Parse;
        d.code = "spice.parse.unknown_card";
        d.message = "unknown card '.lib' (not supported in v1)";
        d.loc = SourceLoc::make("amp.cir", 14, 1);
        CHECK(d.render() == "amp.cir:14:1: error: unknown card '.lib' (not supported in v1)");
    }

    SUBCASE("with analysis context and no location") {
        Diagnostic d;
        d.kind = ErrorKind::Convergence;
        d.analysis = ".dc V1 at 3.30 V";
        d.message = "no convergence after gmin stepping and source stepping";
        CHECK(d.render() ==
              "error: [.dc V1 at 3.30 V] no convergence after gmin stepping and source stepping");
    }

    SUBCASE("names join as a comma list, hints get their own line") {
        Diagnostic d;
        d.kind = ErrorKind::Topology;
        d.message = "voltage-source loop";
        d.names = {"V1", "V2"};
        d.hint = "add series resistance in one branch of the loop";
        CHECK(d.render() ==
              "error: voltage-source loop: V1, V2\n"
              "  hint: add series resistance in one branch of the loop");
    }
}

TEST_CASE("errors cross the boundary as ws::core::Error") {
    Diagnostic d;
    d.kind = ErrorKind::Singular;
    d.message = "matrix is singular";
    const ws::core::Error e = ws::spice::make_error(d);
    CHECK(e.code == static_cast<std::int32_t>(ErrorKind::Singular));
    CHECK(e.category == std::string("spice"));
    CHECK(e.message == std::string("error: matrix is singular"));
    CHECK(static_cast<bool>(e));
}
