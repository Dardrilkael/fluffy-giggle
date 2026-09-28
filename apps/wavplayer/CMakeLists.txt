# Headless ws::audio smoke app: synthesizes a little arpeggio (or loads the
# wav passed as argv[1]) and plays it on the default device. No window, no
# render loop - just ws::core logs and the speakers, exit when the sound is
# done. Desktop-only like the lib it exercises (gated in
# apps/CMakeLists.txt).
ws_add_app(wavplayer DEPENDS ws::audio)
