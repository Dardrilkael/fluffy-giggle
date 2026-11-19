# ws::schematic - the explicit-connectivity wire/move core, the rewrite
# docs/schematic-current.md specifies. THIS library owns the schematic
# CONNECTIVITY MODEL: a node graph (pins, junctions, labels) plus wires
# over node ids, and nets as pure union-find over that graph.
#
# The rules, non-negotiable (the whole reason this lib exists):
#   * GEOMETRY NEVER CREATES CONNECTIVITY. A wire crossing or touching
#     another wire, a wire's path passing over a pin or a junction dot
#     - none of it connects anything. The drawing shows geometry; the
#     graph alone decides what is connected.
#   * A T-CONNECTION IS A JUNCTION NODE THAT SPLITS THE WIRE. There are
#     no "interior attachments" anywhere: no wire keeps a list of dots
#     pinned to its body, no dot keeps a list of wires it taps. A branch
#     drawn onto a wire's body becomes a junction node at that point with
#     the original wire SPLIT into two wires meeting there.
#   * THE OVERLAY IS THE ONE ROUTING CODE PATH. resolve_move(Doc,
#     MoveSession) answers an Overlay - pure data, no mutation, no ui
#     types. The editor's drag PREVIEW draws doc + overlay; the release
#     applies THE SAME OVERLAY as one undo command. Live preview and
#     commit cannot drift because they are the same value.
#
# Everything here is headless C++20 (ws::core's Result/Assert, ws::math's
# DVec2) - no ImGui, no rendering, no editor types; fully unit-testable.
# The editor app adapts its own document to Doc at the seam (editor's
# SchematicAdapter) and owns all input/undo policy.

ws_add_library(schematic DEPENDS ws::core ws::math)

ws_add_test(schematic
  SOURCES tests/test_model.cpp
          tests/test_router.cpp
          tests/test_commands.cpp
  )
