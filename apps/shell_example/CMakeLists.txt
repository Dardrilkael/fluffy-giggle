# apps/shell_example - the stage-15 acceptance app (PLAN_UI_STAGE14_15):
# a text-and-plot "scratchpad" built ONLY from AppShell. The host
# registers two panels (an editor and a plot fed by a background job),
# a document type, a few commands and a settings schema; everything
# else - menus, toolbar, docking, palette, toasts, status bar,
# recents, autosave, session restore, shutdown ordering - is the
# shell's. The direct deps follow the gallery's rule (ws_ui links the
# gui archive PRIVATEly, so the app lists the libs it names itself:
# ws::app's commands/undo, ws::jobs' system, ws::platform's services,
# ws::settings' store).
ws_add_app(shell_example DEPENDS ws::ui ws::jobs ws::platform ws::app ws::settings)
