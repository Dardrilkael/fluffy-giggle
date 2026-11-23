# glyph_probe - text contrast acceptance view.
# One full-screen text rendering test that exercises the shader-based
# contrast curve across theme colors with live gamma/boost overrides.
# No assets: system font only.
ws_add_app(glyph_probe
  SOURCES src/main.cpp
  DEPENDS ws::ui
)
target_compile_definitions(glyph_probe PRIVATE
  NOMINMAX
)