ws_add_app(ui_gallery DEPENDS ws::ui)

# The gallery loads no assets (its font is the system face), so unlike
# playground there is no assets/ directory to ship next to the binary.
