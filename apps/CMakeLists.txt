# Apps are added here, one line each.
add_subdirectory(playground)
# ws::audio is desktop-only in v1 (no on-device backend decision yet), so
# its app is gated the same way.
if(NOT ANDROID)
  add_subdirectory(wavplayer)
endif()
