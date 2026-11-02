# ws::circsim (apps/circsim stage 1): the schematic data model, the .csim
# format, net building, ERC and the ws::spice deck export - headless by
# construction, no ui include anywhere. ws::spice is the ONLY simulator.
#
# The settings dep is deliberate: ws::settings::json is the workspace's
# insertion-order, unknown-field-preserving JSON (the .csim carrier) and
# ws::settings::FileSystem is the atomic-write seam every persistence user
# rides. ws::math rides PUBLIC for ws::math::DVec2 (model.hpp positions).
ws_add_library(circsim DEPENDS ws::core ws::math ws::spice ws::settings)

ws_add_test(circsim SOURCES tests/test_format.cpp tests/test_nets.cpp
                              tests/test_erc.cpp tests/test_export.cpp
                              tests/test_symbols.cpp)
