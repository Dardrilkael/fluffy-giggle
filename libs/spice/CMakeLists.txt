# The headless circuit simulator (libs/spice/DESIGN.md): netlist parser,
# elaboration with subckt flattening and topological checks, MNA assembly
# over the ws::math sparse LU facade, and the analysis drivers (op, dc -
# tran/ac/noise follow in M2+). Pure std + ws::core on the public surface.
ws_add_library(spice DEPENDS ws::core)

# ws::math is a workspace lib (the sparse LU facade backing the MNA solver)
# but linked PRIVATE: no ws::math type ever appears under include/ - the
# workspace rule, spelled out for this exact case in DESIGN.md §2.2.
target_link_libraries(ws_spice PRIVATE ws::math)

ws_add_test(spice SOURCES tests/test_spice.cpp tests/test_parse.cpp)
