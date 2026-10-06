# Apps are added here, one line each.
add_subdirectory(playground)
# chatter is a gui app too (netchat's protocol behind a palette gui, built
# for the phone and the desktop alike), so it stays ungated like
# playground - only the console apps below are desktop-only.
add_subdirectory(chatter)
# ws::audio builds on Android too (its engine pimpl is OpenAL Soft on both
# platforms), but wavplayer is a desktop CONSOLE app - a main() that pumps
# update()/is_playing() with stdout logging - so the app stays gated.
if(NOT ANDROID)
  add_subdirectory(wavplayer)
  # netdemo is the same kind of console app - ws::net traffic narrated
  # through ws::core logs.
  add_subdirectory(netdemo)
  # netchat too: several terminals (or machines) find each other over
  # discovery and exchange framed text over TCP.
  add_subdirectory(netchat)
  # bench_physics is the fourth console app: steps a falling-boxes scene in
  # ws::physics and prints per-tick wall-clock cost per dimension/backend.
  add_subdirectory(bench_physics)
  # bounce is the first windowed example to compose three libs: a
  # ws::physics2d pit drawn through ws::ui, with ws::audio thuds on impact.
  add_subdirectory(bounce)
endif()
