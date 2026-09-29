add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(ui)
# ws::audio builds everywhere: the engine pimpl is OpenAL Soft on both
# platforms, and OpenAL Soft's own device layer goes AAudio-via-Oboe on
# Android (cmake/ThirdParty.cmake fetches Oboe there, PRIVATE like every
# third-party).
add_subdirectory(audio)
