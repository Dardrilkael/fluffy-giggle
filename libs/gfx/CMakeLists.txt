# ws::gfx - the Vulkan device layer promoted out of ws::ui (stage 3b):
# the loader/instance, the surface-via-callback, physical-device/queue
# selection, the device, the VMA allocator. volk/vma/vulkan-headers are
# PRIVATE like every third-party: the public headers are ws-spelled only
# and handles cross as opaque void*, so no Vulkan header can leak. ws::ui
# is the only consumer; apps never include this lib.
ws_add_library(gfx DEPENDS ws::core)

# vma's implementation TU lives here (src/vma_impl.cpp, moved from ui) -
# since stage 3b this lib is the workspace's single VMA user.
# vulkan-headers rides volk (PUBLIC there); vma rides both.
target_link_libraries(ws_gfx PRIVATE volk vma)

# The pure device-choice and swapchain-choice helpers (the tier decision,
# the depth-format walk, the swapchain tables) are tested headless through
# the internal headers; no GPU is faked and no volk entry point runs on
# any tested path.
ws_add_test(gfx SOURCES tests/test_gfx.cpp)
# test_gfx instantiates Gpu and Swapchain (their null-state contracts):
# the archive's objects reference volk globals at link time even though
# the tested paths never call them.
if(TARGET test_gfx)
  target_include_directories(test_gfx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_gfx PRIVATE volk)
endif()
