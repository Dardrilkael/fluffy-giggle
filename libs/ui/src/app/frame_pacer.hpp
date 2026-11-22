// frame_pacer.hpp - the idle-frame pacing POLICY, as pure data in, one
// decision out (no window, no clock of its own, no GPU).
//
// The stock app loop renders every iteration: NewFrame, on_update, the
// r2d replay, present - paced only by the swapchain's vsync. An immediate-
// mode UI that is sitting still pays for all of it, sixty times a second,
// to redraw the same picture. This decides, once per iteration, whether
// the frame is worth drawing; the loop owns the rest (it blocks in the
// platform's wait instead of spinning when the answer is no).
//
// The rules, all of them conservative - a frame skipped wrongly is a bug a
// user sees, a frame rendered needlessly is only the old behavior:
//
//  - DISABLED (the default) renders every iteration, exactly as before.
//  - ACTIVITY renders: a platform event this iteration, a job completion
//    or progress delivery, an input button/key/touch still held, the app
//    asking for a frame (an animation it runs), a swapchain rebuild owed.
//  - SETTLE: an immediate-mode UI answers an input one frame late (the
//    hot/active widget settles after the frame that saw the press), so
//    each burst of activity owes `settle_frames` more frames after the
//    last active one, active or not.
//  - HEARTBEAT: an idle loop still renders every `heartbeat_seconds`.
//    Time-driven things nothing announces - a tooltip's hover delay, a
//    caret blink, a toast's timeout, an exposed window with no
//    compositor - keep ticking at that rate, and a worker thread's
//    result waits at most one heartbeat for the loop to look.
//
// Header-only so the policy is testable without the app loop (which needs
// Vulkan and a window): tests/test_frame_pacer.cpp drives it by hand.
#pragma once

#include <algorithm>

namespace ws::ui {

struct FramePacerConfig {
    bool enabled = false;
    // An idle loop renders at least this often (seconds).
    double heartbeat_seconds = 0.1;
    // Frames still owed after the last active iteration.
    int settle_frames = 3;
};

// What this iteration found. Every flag means "draw a frame".
struct FrameActivity {
    bool events = false;         // a platform event arrived
    bool jobs = false;           // the job drain delivered something
    bool input_held = false;     // a button / key / touch is down
    bool app_wants = false;      // the app's own animation / playback
    bool surface_dirty = false;  // a swapchain rebuild is pending

    [[nodiscard]] bool any() const {
        return events || jobs || input_held || app_wants || surface_dirty;
    }
};

class FramePacer {
public:
    explicit FramePacer(const FramePacerConfig& config = {}) : cfg_(config) {}

    // Once per loop iteration, after its events and jobs were collected.
    // `now` is seconds on any monotonic clock. True = draw this frame.
    [[nodiscard]] bool should_render(const FrameActivity& activity,
                                     double now) {
        if (!cfg_.enabled) {
            last_render_ = now;
            rendered_ever_ = true;
            idle_ = false;
            return true;
        }
        const bool active = activity.any();
        if (active) {
            settle_ = cfg_.settle_frames;
        }
        const bool heartbeat_due =
            !rendered_ever_ || now - last_render_ >= cfg_.heartbeat_seconds;
        const bool render = active || settle_ > 0 || heartbeat_due;
        if (render) {
            if (!active && settle_ > 0) {
                --settle_;  // a settle frame, spent
            }
            last_render_ = now;
            rendered_ever_ = true;
        }
        idle_ = !render;
        return render;
    }

    // True when the last decision was "skip": the loop should block in the
    // platform's wait (for `idle_wait`) instead of polling.
    [[nodiscard]] bool idle() const { return idle_; }

    // How long the loop may block before the heartbeat is due (seconds).
    // Never zero while idle - a zero wait is a spin - and never past the
    // heartbeat, so an event-less wait ends in a frame.
    [[nodiscard]] double idle_wait(double now) const {
        constexpr double kMinWait = 0.001;
        if (!cfg_.enabled || !rendered_ever_) {
            return 0.0;
        }
        return std::max(cfg_.heartbeat_seconds - (now - last_render_),
                        kMinWait);
    }

    [[nodiscard]] const FramePacerConfig& config() const { return cfg_; }

private:
    FramePacerConfig cfg_;
    double last_render_ = 0.0;
    bool rendered_ever_ = false;
    bool idle_ = false;
    int settle_ = 0;
};

}  // namespace ws::ui
