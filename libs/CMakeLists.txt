add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(ui)
# ws::audio is desktop-only in v1: its device backend (OpenAL Soft) is
# fetched desktop-only (cmake/ThirdParty.cmake), so the lib stays off the
# Android build until an on-device backend decision lands
# (libs/audio/CLAUDE.md).
if(NOT ANDROID)
  add_subdirectory(audio)
endif()
