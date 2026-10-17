add_subdirectory(core)
add_subdirectory(math)
# ws::input is the input family promoted out of ui (stage 3a): the event
# types + the InputFrame snapshot, pure CPU and GLFW-free (codes ride as
# plain ints). ws::ui's platform TUs are the only writers of an InputFrame,
# so ui links this lib - never the other way around.
add_subdirectory(input)
# ws::gfx is the Vulkan device layer promoted out of ui (stage 3b): the
# loader/instance, the surface-via-callback, device/queue selection, the
# VMA allocator. volk/vma stay PRIVATE here (handles cross to ui as
# opaque void*), and ui is the only consumer - never the other way
# around.
add_subdirectory(gfx)
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
