# ws::jobs and ws::platform are named alongside ws::ui because the
# gallery's pages include their headers directly (the jobs section
# submits the counting job; the platform page is stage 12's bridge:
# the dialogs, the MRU, the autosave pair, the clipboard, the title,
# the single-instance grab); ws::ui already carries the jobs bridge
# itself transitively. ws::app is named for the same reason: the
# commands page composes the registry, the resolver, the keymap file
# and the undo stack directly (stage 13), not just through the gui's
# command views.
ws_add_app(ui_gallery DEPENDS ws::ui ws::jobs ws::platform ws::app)

# The gallery loads no assets (its font is the system face), so unlike
# playground there is no assets/ directory to ship next to the binary.
