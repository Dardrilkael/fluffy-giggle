ws_add_app(playground DEPENDS ws::ui)

# The 2D demo loads assets/checker.png relative to the exe's working
# directory, so ship the assets next to the binary. A missing file is a
# documented fallback (white texture + one log line), not an error.
add_custom_command(TARGET playground POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:playground>/assets")
