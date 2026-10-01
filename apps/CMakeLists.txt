# Apps are added here, one line each.
add_subdirectory(playground)
# ws::audio builds on Android too (its engine pimpl is OpenAL Soft on both
# platforms), but wavplayer is a desktop CONSOLE app - a main() that pumps
# update()/is_playing() with stdout logging - so the app stays gated.
if(NOT ANDROID)
  add_subdirectory(wavplayer)
  # netdemo is the same kind of console app - ws::net traffic narrated
  # through ws::core logs.
  add_subdirectory(netdemo)
endif()
