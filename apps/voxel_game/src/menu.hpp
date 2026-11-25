// apps/voxel_game - menu.hpp: the pause menu's pure geometry and hit
// test, engine-free (no ws:: includes) so the logic suite can pin it
// headless exactly the way world.hpp's rules are pinned. The app side
// (main.cpp) owns the state: paused_ gates the simulation and the
// cursor mode; this header only answers "where do the buttons sit"
// and "which button, if any, did the click land in".
//
// The layout is the one main.cpp renders: two buttons centered on the
// frame, resume above quit, the title "paused" above them. The
// hit test is inclusive on the left/top edge, exclusive on the
// right/bottom (the [x, x+w) convention every rect API uses), so two
// adjacent buttons share no pixel and a click between them hits
// neither.

#pragma once

namespace voxel::menu {

// One menu button in gui-space pixels.
struct Button {
    float x, y, w, h;
};

constexpr int kButtonCount = 2;  // resume, quit
constexpr float kButtonW = 240.0f;
constexpr float kButtonH = 44.0f;
constexpr float kButtonGap = 14.0f;

// The two buttons, centered horizontally, vertically centered as a
// pair: resume on top, quit below. `out` takes kButtonCount entries.
inline void layout(float frame_w, float frame_h, Button out[kButtonCount]) {
    const float x = (frame_w - kButtonW) * 0.5f;
    const float pair_h = kButtonCount * kButtonH + (kButtonCount - 1) * kButtonGap;
    const float y0 = frame_h * 0.5f - pair_h * 0.5f;
    out[0] = Button{x, y0, kButtonW, kButtonH};                       // resume
    out[1] = Button{x, y0 + kButtonH + kButtonGap, kButtonW, kButtonH};  // quit
}

// The [x, x+w) hit: left/top inclusive, right/bottom exclusive.
inline bool hit(const Button& b, float x, float y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// Which button a click at (x, y) lands in: 0 = resume, 1 = quit, and
// -1 = none (a click outside the buttons - the modal shade owns the
// whole screen, so the app eats it but changes nothing).
inline int click_action(float frame_w, float frame_h, float x, float y) {
    Button buttons[kButtonCount];
    layout(frame_w, frame_h, buttons);
    for (int i = 0; i < kButtonCount; ++i) {
        if (hit(buttons[i], x, y)) {
            return i;
        }
    }
    return -1;
}

}  // namespace voxel::menu
