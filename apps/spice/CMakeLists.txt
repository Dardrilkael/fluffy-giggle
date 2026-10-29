# The ws::spice CLI: a thin parse -> simulate -> write shell (DESIGN.md
# §0.6). argv, file I/O and nothing else - every real behavior lives in the
# lib. Desktop console app like wavplayer/netchat (gated in
# apps/CMakeLists.txt).
ws_add_app(spice DEPENDS ws::spice)

# The golden-deck smoke suite (tools/smoke_spice.py): analytic .op/.dc/
# .tran/.ac decks driven through the real CLI, asserting closed-form
# values, headers, row counts and exit codes. Cross-platform (Python, not
# PowerShell) so it runs in CI on Linux too. Registered as a ctest entry
# whenever the spice app is built and tests are on; mirrors the Windows-only
# tools/smoke-spice.ps1 - keep the two in sync when adding cases.
if(WS_BUILD_TESTS AND NOT ANDROID AND TARGET spice)
  find_program(_python3 NAMES python3 python)
  if(_python3)
    add_test(NAME spice_smoke
      COMMAND ${_python3}
              "${CMAKE_SOURCE_DIR}/tools/smoke_spice.py"
              --spice "$<TARGET_FILE:spice>")
  endif()
endif()
