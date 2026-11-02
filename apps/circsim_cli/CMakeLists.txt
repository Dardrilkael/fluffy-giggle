# The ws::circsim CLI (desktop console app): load a .csim, run an analysis
# through ws::circsim -> ws::spice, write CSV/JSON; `erc` and `netlist`
# check and inspect without simulating. argv, file I/O and exit codes only.
ws_add_app(circsim_cli DEPENDS ws::circsim)

# The golden-schematic smoke suite (tools/smoke_circsim.py): the two
# shipped examples driven through the real CLI, asserting closed-form
# values and exit codes - the "circsim_cli runnable from stage 1 on" gate.
if(WS_BUILD_TESTS AND NOT ANDROID AND TARGET circsim_cli)
  find_program(_python3 NAMES python3 python)
  if(_python3)
    add_test(NAME circsim_smoke
      COMMAND ${_python3}
              "${CMAKE_SOURCE_DIR}/tools/smoke_circsim.py"
              --cli "$<TARGET_FILE:circsim_cli>")
  endif()
endif()
