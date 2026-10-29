# A tiny example app: hardcoded RC-transient deck -> run -> print the
# v(out) column + the .meas result. The smallest possible ws::spice
# embedding - no argv, no file I/O, just the lib calls.
ws_add_app(spice_example DEPENDS ws::spice)
