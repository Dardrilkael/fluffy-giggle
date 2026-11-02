# apps/circsim_editor - the circsim stage-3 acceptance app (docs/
# CIRCSIM_PLAN.md): the schematic editor built ONLY from AppShell over
# libs/circsim - five panels (the schematic canvas through ws::ui's
# canvas layer, the symbol palette, the property grid over ParamSpecs,
# the problems list with select-and-zoom through the export back-map,
# the netlist view), the .csim document type whose every edit rides the
# undo stack, and the simulate job over run_schematic. Stage 4 adds the
# Results panel (the plot over ws::spice's tables through
# ws::circsim's axis math, the hover crosshair, the CSV/JSON export).
# The direct deps follow the shell_example's rule (ws::ui links the gui
# archive PRIVATEly, so the app lists the libs it names itself).
ws_add_app(circsim_editor DEPENDS ws::ui ws::circsim ws::jobs ws::platform
                               ws::app ws::settings)

# The stage-4 headless gate: the Results panel driven the way the shell
# drives it (a GuiContext frame + a panel rect), with a
# CommandListCanvas injected at the Editor's plot_canvas seam asserting
# the draw geometry. The link shape is the ui suites' own: the gui
# archive (canvas + widgets + shell, no Vulkan/GLFW anywhere) plus the
# r2d stub and the real pure-CPU font bake the gui archive's undefined
# r2d/font symbols resolve against. main.cpp/canvas_panel/side_panels
# stay out - the app TU owns the App subclass and the window.
ws_add_test(circsim_editor_panel
  SOURCES tests/test_results_panel.cpp
          src/editor.cpp
          src/document.cpp
          src/results_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb)
if(TARGET test_circsim_editor_panel)
  # "editor.hpp"/"results_panel.hpp" from the app's src, the stub's
  # "ui_gui_stub.hpp" and font.cpp's "text/font.h" from libs/ui's src.
  target_include_directories(test_circsim_editor_panel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src")
endif()
