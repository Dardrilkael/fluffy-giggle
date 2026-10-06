# apps/bounce - the audio + ui + physics composition example: a clickable
# "pit" of boxes and circles simulated by ws::physics2d, drawn through
# ws::ui's r2d renderer with render interpolation, controlled from a
# ws-gui panel, and scored by ws::audio one-shot thuds whose pitch and
# gain follow each impact's speed.
ws_add_app(bounce DEPENDS ws::physics ws::audio ws::ui)
