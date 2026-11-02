# apps/circsim_editor - the circsim stage-3 acceptance app (docs/
# CIRCSIM_PLAN.md): the schematic editor built ONLY from AppShell over
# libs/circsim - five panels (the schematic canvas through ws::ui's
# canvas layer, the symbol palette, the property grid over ParamSpecs,
# the problems list with select-and-zoom through the export back-map,
# the netlist view), the .csim document type whose every edit rides the
# undo stack, and the simulate job over run_schematic. The direct deps
# follow the shell_example's rule (ws::ui links the gui archive
# PRIVATEly, so the app lists the libs it names itself).
ws_add_app(circsim_editor DEPENDS ws::ui ws::circsim ws::jobs ws::platform
                               ws::app ws::settings)
