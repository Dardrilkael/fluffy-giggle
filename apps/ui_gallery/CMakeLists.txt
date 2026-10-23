# ws::jobs is named alongside ws::ui because the gallery's jobs section
# includes its headers directly (the demo submits the counting job);
# ws::ui already carries the bridge itself transitively.
ws_add_app(ui_gallery DEPENDS ws::ui ws::jobs)

# The gallery loads no assets (its font is the system face), so unlike
# playground there is no assets/ directory to ship next to the binary.
