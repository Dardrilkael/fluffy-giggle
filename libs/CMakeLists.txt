add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(ui)
# ws::audio builds everywhere: the engine pimpl is OpenAL Soft on both
# platforms, and OpenAL Soft's own device layer goes AAudio-via-Oboe on
# Android (cmake/ThirdParty.cmake fetches Oboe there, PRIVATE like every
# third-party).
add_subdirectory(audio)

# ws::net is cross-platform on the same libuv backend everywhere: TCP/UDP +
# LAN discovery, uv private to src/backend/uv (the glfw/openal fetch
# pattern, unconditional).
add_subdirectory(net)

# ws::physics is cross-platform on the same two backends everywhere:
# Box2D (2D) + Jolt (3D), both private to src/backend and both fetched
# unconditionally - the 2D and 3D surfaces build identically on Android.
add_subdirectory(physics)
