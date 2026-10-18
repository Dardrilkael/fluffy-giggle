#pragma once

#include <cstdint>
#include <memory>

#include <ws/core/result.hpp>

namespace ws::gfx {

class Gpu;

// How many frames may overlap on the GPU: the frame-slot count, decoupled
// from the swapchain image count (which only bounds how many images exist).
// The consumer's renderer keys its per-slot state by the frame_index of
// the acquired frame - the shell static_asserts its own constant equal to
// this one so the two can never drift apart.
inline constexpr uint32_t kMaxFramesInFlight = 2;

// Present preference, applied at every (re)create: vsync on is FIFO (the
// one mode the spec guarantees); vsync off prefers mailbox (still
// tear-free), then immediate, then FIFO.
struct SwapchainOptions {
    bool vsync = true;
};

// acquire()'s answer. Any other failure - device loss mid-frame and the
// like - logs and aborts: after a failed acquire/submit there is no state
// to roll back to (semaphores in flight, a command buffer half recorded).
enum class AcquireStatus {
    ok,          // frame may render (a suboptimal acquire still renders)
    out_of_date  // bail: drop the frame, recreate before the next acquire
};

// present()'s answer; the frame just presented is fully retired either way.
enum class PresentStatus {
    ok,
    needs_rebuild  // out-of-date/suboptimal at present, or a suboptimal acquire
};

// The swapchain and everything tied to its extent: image views, the depth
// attachment, the color+depth render pass, framebuffers, and the frame sync
// - kMaxFramesInFlight frame slots (fence, command pool+buffer, acquire
// semaphore each) plus one render-finished semaphore per image. Promoted
// out of ws::ui's app shell (stage 3b); ws::ui stays the only consumer.
//
// Handles cross the boundary as opaque void* exactly like Gpu's (no Vulkan
// vocabulary may appear in a signature); the consumer unwraps at its own
// single cast point. Creation and teardown return core::Status - the
// consumer logs it. The Gpu must be attached (surface + device stage up)
// at create(), and must outlive the Swapchain.
class Swapchain {
public:
    // One acquired frame: filled by acquire(), handed back to every
    // frame-flow call. The command buffer is the one the caller records
    // into between begin_render_pass()/end_render_pass() (the consumer's
    // renderer and UI backend take it as their recording target).
    struct Frame {
        uint32_t image_index = 0;        // swapchain image this frame renders into
        uint32_t frame_index = 0;       // frame slot (per-slot renderer state keys on it)
        void* command_buffer = nullptr;  // VkCommandBuffer
    };

    Swapchain();
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Window-stage create against an attached Gpu. fb_width/fb_height is
    // the extent fallback for surfaces with no fixed size. A failure
    // cleans up after itself - destroy() is safe on the partial state.
    core::Status create(Gpu& gpu, int fb_width, int fb_height,
                        const SwapchainOptions& options);
    // Idempotent; safe on partial state, before create(), and after a
    // failed create(). The Gpu/options from create() stay - create() can
    // run again (the re-attach path) without re-passing them.
    void destroy();
    // Recreate after a resize/out-of-date with the options from create():
    // destroy() then build() in one call.
    core::Status recreate(int fb_width, int fb_height);

    // --- frame flow ---------------------------------------------------------
    // Waits the slot fence (the CPU throttle), acquires an image, then -
    // only after the acquire succeeded - resets the fence and command pool
    // and begins the frame's command buffer. An out-of-date swapchain
    // answers out_of_date with NOTHING acquired or signaled: the caller
    // drops its frame and recreates; the slot stays signaled and pristine,
    // so no later acquire can wait a fence nobody submits against. A
    // suboptimal acquire is a success - the image IS acquired and its
    // semaphore IS signaled, so the frame must run and present; only a
    // submit's semaphore wait retires that signal, and present() folds the
    // rebuild in once the frame has retired.
    AcquireStatus acquire(Frame* out);
    // vkCmdBeginRenderPass on the frame's framebuffer: the color+depth
    // render pass (its two external subpass dependencies are what make
    // the single shared depth image safe across frames in flight) with
    // the caller's clear color; depth always clears to 1.0 - far plane,
    // the LEQUAL convention.
    void begin_render_pass(const Frame& frame, const float clear_color[4]);
    void end_render_pass(const Frame& frame);
    // Ends the buffer and submits: waits the acquire semaphore at the
    // color-attachment stage (nothing earlier touches the image), signals
    // this image's render-finished semaphore for its present, and the
    // slot's fence for the next user of the slot.
    void submit(const Frame& frame);
    // Presents the frame's image (waits its render-finished semaphore).
    PresentStatus present(const Frame& frame);

    // Wait every slot fence - all submitted draw work retired, without
    // idling the whole device. The consumer's dev-only pipeline-rebake
    // seam (every submitted draw lives in a slot submission).
    void wait_frames();

    // --- opaque state (valid between create/recreate and destroy) ----------
    void* render_pass() const;       // VkRenderPass
    uint32_t format() const;         // VkFormat, numeric
    uint32_t present_mode() const;   // VkPresentModeKHR, numeric
    uint32_t image_count() const;
    uint32_t width() const;   // the current extent
    uint32_t height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::gfx
