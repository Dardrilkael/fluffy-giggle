# apps/circsim_editor - the circsim stage-3 acceptance app (docs/
# CIRCSIM_PLAN.md): the schematic editor built ONLY from AppShell over
# libs/circsim - five panels (the schematic canvas through ws::ui's
# canvas layer, the symbol palette, the property grid over ParamSpecs,
# the problems list with select-and-zoom through the export back-map,
# the netlist view), the .csim document type whose every edit rides the
# undo stack, and the simulate job over run_schematic. Stage 4 adds the
# Results panel (the plot over ws::spice's tables through
# ws::circsim's axis math, the hover crosshair, the CSV/JSON export).
# Stage 5 gives the plot its hands: the wheel zoom, the rubber-band box
# zoom, the axis-drag zooms, the pan, the double-click reset - all
# riding ws::circsim/plot.hpp's clamp/zoom/pan math.
# The direct deps follow the shell_example's rule (ws::ui links the gui
# archive PRIVATEly, so the app lists the libs it names itself).
ws_add_app(circsim_editor DEPENDS ws::ui ws::circsim ws::jobs ws::platform
                               ws::app ws::settings ws::schematic
                               ws::gestures)

# The stage-4 headless gate: the Results panel driven the way the shell
# drives it (a GuiContext frame + a panel rect), with a
# CommandListCanvas injected at the Editor's plot_canvas seam asserting
# the draw geometry. The link shape is the ui suites' own: the gui
# archive (canvas + widgets + shell, no Vulkan/GLFW anywhere) plus the
# r2d stub and the real pure-CPU font bake the gui archive's undefined
# r2d/font symbols resolve against. main.cpp/canvas_panel/side_panels
# stay out - the app TU owns the App subclass and the window.
# The parity suite (the unify-plotting merge's own gate): identical
# data and viewport configurations fed through BOTH consumers' public
# plotting APIs - the gui widget's (ws::ui::gui::plot_*) and the
# Circsim panel's (ws::circsim::plot through its facade) - asserting
# the answers agree. No production TU may hold both tiers (that is
# the layering the merge keeps); a TEST is exactly where the two
# spellings meet, and this one makes them stay one decision set.
# Links the gui archive (the widget's pure math is public in
# widgets.hpp) beside ws::circsim: no Editor, no canvas, no app TU -
# the pure calls alone.
ws_add_test(circsim_editor_plot_parity
  SOURCES tests/test_plot_parity.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::plotmath stb)
if(TARGET test_circsim_editor_plot_parity)
  target_include_directories(test_circsim_editor_plot_parity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/tests")
endif()

# The PNG/live consistency suite (the E3 export's own gate): the
# render_plot_png path driven headless, asserted through a small PNG
# decoder on the export's own deterministic bytes (the encoder's
# filter-0/fixed-Huffman contract, parseable without linking the
# canvas) - the spine split, the run segmentation, the envelope fit,
# the markers, the hidden-signal exclusion, and the shared-math
# parity with the live home fit.
ws_add_test(circsim_editor_plot_png
  SOURCES tests/test_plot_png.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/document.cpp
          src/results_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::plotmath stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_plot_png)
  target_include_directories(test_circsim_editor_plot_png PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/tests")
endif()

ws_add_test(circsim_editor_panel
  SOURCES tests/test_results_panel.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/schematic_adapter.cpp
          src/document.cpp
          src/results_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_panel)
  # "editor.hpp"/"results_panel.hpp" from the app's src, the stub's
  # "ui_gui_stub.hpp" and font.cpp's "text/font.h" from libs/ui's src
  # (the .four section's rows read the r2d stub the side-panels suite
  # reads - mono-face rows count themselves there too).
  target_include_directories(test_circsim_editor_panel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/tests")
endif()

# The Schematic panel's headless gate: the camera mapping. The panel
# draws through the Editor's canvas_panel_canvas seam (same pattern as
# the plot seam above); the suite asserts every element's recorded
# screen geometry lands where EditorView::w2s answers - the contract
# the grid, the snap feedback and the picks all ride (the panned-
# camera case is the regression: the panel once drew its world offset
# by center * zoom the moment the camera left the origin).
ws_add_test(circsim_editor_canvas
  SOURCES tests/test_canvas_panel.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/schematic_adapter.cpp
          src/document.cpp
          src/canvas_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_canvas)
  target_include_directories(test_circsim_editor_canvas PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src")
endif()

# The Schematic panel's frame-cost regressions.
ws_add_test(circsim_editor_canvas_cost
  SOURCES tests/test_canvas_cost.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/schematic_adapter.cpp
          src/document.cpp
          src/canvas_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_canvas_cost)
  target_include_directories(test_circsim_editor_canvas_cost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src")
endif()

# The side panels' headless gate: the Properties panel's editing state
# driven the same way (a GuiContext frame + a panel rect) - the
# directives split/join round-trip (an emptied last row must survive a
# reload), the analysis list's remove guard (nothing selected removes
# nothing), the field rows' "##" identity (a param labeled "value" is
# its own widget, not the built-in value row - the focus-navigation
# walk proves two stops where the shared-label bug fused them into
# one), refresh_problems' snapshot version, and the Palette's draw
# seam. side_panels groups its symbols through ws::circsim's palette
# (src/palette_groups.cpp moved to the lib - stage 11, the lib
# extraction; the panel is the app's drawing over it); the link shape
# is the suites' own again. The typography
# pass's cases read the r2d stub the palette/problems rows draw
# through, so the stub header's dir rides the include path.
ws_add_test(circsim_editor_side_panels
  SOURCES tests/test_side_panels.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/schematic_adapter.cpp
          src/document.cpp
          src/side_panels.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_side_panels)
  target_include_directories(test_circsim_editor_side_panels PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/tests")
endif()

# The waveform render profiling gate: the Results panel's series loop
# driven through Renderer2DCanvas (the real CPU tessellator) with the
# r2d stub recording draw calls. Opt-in via CIRCSIM_PLOT_PROF=1 (logs
# per-window prep/stroke/heap averages); CIRCSIM_PLOT_JOIN=bevel|miter
# selects the join style (default round). This is a smoke test without
# the env vars; with them it produces the A/B numbers for the join
# comparison.
ws_add_test(circsim_editor_waveform_prof
  SOURCES tests/test_waveform_prof.cpp
          src/schematic_adapter.cpp
          src/editor.cpp
          src/schematic_adapter.cpp
          src/document.cpp
          src/results_panel.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/tests/ui_gui_r2d_stub.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/font.cpp
          ${PROJECT_SOURCE_DIR}/libs/ui/src/text/stb_impl.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings stb ws::schematic
          ws::gestures)
if(TARGET test_circsim_editor_waveform_prof)
  target_include_directories(test_circsim_editor_waveform_prof PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/src"
    "${PROJECT_SOURCE_DIR}/libs/ui/tests")
endif()

# The document undo regression gate. These tests exercise deletion snapshots
# and deque restoration directly, without depending on the canvas test fixture.
ws_add_test(circsim_editor_document
  SOURCES tests/test_document_undo.cpp
          src/schematic_adapter.cpp
          src/document.cpp
  LINK ws::circsim
  DEPENDS ws::ui_gui ws::app ws::settings ws::schematic)

# The visual identity's gate: both app chromes pass the library's WCAG
# pair checks and every device ink stays legible on its canvas. Pure
# data - only the gui archive's theme_color math is linked.
ws_add_test(circsim_editor_style
  SOURCES tests/test_ui_style.cpp
  LINK ws::ui_gui
  DEPENDS ws::ui_gui)
