ws_add_app(playground DEPENDS ws::ui)

if(NOT ANDROID)
  # The sound panel exercises ws::audio, desktop-only in v1 (no on-device
  # backend decision yet) - the link and the panel (#ifndef __ANDROID__ in
  # src/main.cpp) share this gate.
  target_link_libraries(playground PRIVATE ws::audio)

  # The 2D demo loads assets/checker.png relative to the exe's working
  # directory, so ship the assets next to the binary. A missing file is a
  # documented fallback (white texture + one log line), not an error.
  # Android reads the same assets/ out of the APK instead:
  # tools/package-android.ps1 packs them, src/main.cpp unpacks at startup.
  add_custom_command(TARGET playground POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:playground>/assets")
endif()
