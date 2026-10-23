#pragma once

// The ws::jobs bridge (nextplan stage 8): the gui-side halves of the
// job system's story. ws::jobs is pure std + core - it marshals
// progress and completions to the DRAIN thread and knows nothing about
// windows or frames - and this header is the translation into the gui's
// caller-owned-state contract:
//
//   JobProgress        one job's progress mirror, filled by the drain
//                      (App::install_job_system puts that drain between
//                      frames, on the main thread)
//   job_view()         the pure state-to-view mapping - JobState plus
//                      the mirror into bar fill, face, word, cancel -
//                      the scripted-states test seam
//   job_progress()     the row widget: "label: state" line, the
//                      state-colored bar, the cancel button wired to
//                      request_cancel()
//   toast_on_failure() the completion helper: a failed job posts one
//                      error toast; cancelled is not a failure
//
// The mirror is PLAIN state on purpose: the App seam guarantees the
// drain and the gui run on one thread, so report() needs no lock - the
// same single-thread guarantee Toasts and every other caller-owned
// store already ride. A mirror used outside that seam (a drain on some
// other thread) is out of contract.

#include <ws/core/result.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/jobs/job_system.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ws::ui::gui {

// One job's progress, as the drain delivers it: the latest fraction
// (clamped to [0,1] - the same clamp the record applies, so a mirror
// filled by hand cannot draw past the end) and the latest message
// (copied out on report; the callback's pointer dies with the call, the
// same lifetime rule the drain's delivery documents). One mirror per
// job; reset() when the handle slot is reused for a new job, or the
// new run starts with the old one's last report.
class JobProgress {
public:
    // The on_progress half of the drain: clamp and store.
    void report(float fraction, const char* message);

    // The callbacks shape for a job whose progress this mirror shows:
    // on_progress wired to report(), on_complete left null for the
    // caller to fill (the gallery's is toast_on_failure plus a status
    // echo). The wired lambda captures `this`, so the mirror must
    // outlive the job's callbacks - which reset() bounds: a new job on
    // the same mirror rewires anyway.
    template <class T>
    ws::jobs::JobCallbacks<T> callbacks();

    [[nodiscard]] float fraction() const { return fraction_; }
    [[nodiscard]] const std::string& message() const { return message_; }

    // Back to "nothing reported": the fraction an empty bar shows.
    void reset();

private:
    float fraction_ = 0.0f;
    std::string message_;
};

// The state-to-view mapping, pulled out as a pure function because it
// IS the widget's whole look: which fraction the bar shows, which face
// the fill wears, which word rides the label, whether the cancel
// button is live. The terminal states land a FULL bar whatever the
// mirror says - a finished row shows its outcome, not its last report;
// failed wears the toast-error red so the row and the toast it posts
// read as one story.
struct JobView {
    float fraction = 0.0f;   // the bar's fill, [0,1]
    std::uint32_t fill = 0;  // the bar fill's face (progress()'s
                             // control_on slot, overridden per state)
    const char* word = "";   // the label's state word - "running" or
                             // the mirror's latest message while live
    bool cancellable = false;  // the cancel button's live flag
};

// The scripted-states seam: no window, no drain, no job system - state
// in, look out. cancelled keeps the mirror's fraction (a half-run job
// that was cancelled stays visibly half-run) but the disabled face.
[[nodiscard]] JobView job_view(ws::jobs::JobState state,
                               const JobProgress& mirror,
                               const ChromeTheme& theme);

// The job row, one handle plus its mirror: "label: state" on the first
// line, the state-colored bar and its cancel button on the second.
// The bar's color rides control_on - progress()'s fill face - through
// a local theme copy, so each state wears its own without progress()
// growing a color parameter. Returns true the frame the cancel button
// was clicked; the request lands on the job immediately and the state
// follows when the job honours it (a null handle draws the row's idle
// look - "no job", empty bar, dead cancel - which is how a page keeps
// its layout before the first submit).
template <class T>
bool job_progress(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label,
                  const ws::jobs::JobHandle<T>& handle,
                  const JobProgress& mirror) {
    const JobView view = job_view(handle.state(), mirror, theme);

    // The label line: "label: word" - the running word is the mirror's
    // latest message, and text()'s own fit truncates whatever runs
    // past the content width.
    char line[160];
    std::snprintf(line, sizeof line, "%s: %s", label, view.word);
    text(layout, theme, line, layout.remaining_width());

    // The bar row: the bar fills everything but the cancel button, the
    // button rides the same line. A dead cancel (the terminal states)
    // draws through button()'s own disabled path.
    constexpr float cancel_w = 80.0f;
    const float bar_w =
        std::max(0.0f, layout.remaining_width() - cancel_w - theme.spacing);
    ChromeTheme bar_theme = theme;
    bar_theme.control_on = view.fill;
    progress(layout, bar_theme, view.fraction, bar_w);
    layout.same_line();
    if (button(gui, layout, theme, "cancel", cancel_w, {},
               !view.cancellable)) {
        handle.request_cancel();
        return true;
    }
    return false;
}

template <class T>
ws::jobs::JobCallbacks<T> JobProgress::callbacks() {
    ws::jobs::JobCallbacks<T> cbs;
    cbs.on_progress = [this](float fraction, const char* message) {
        report(fraction, message);
    };
    return cbs;
}

// The completion-side helper, for on_complete: takes the job's Result;
// a FAILED job formats its error into `text` and posts exactly one
// error toast. `text` is the caller-owned long-lived buffer the toast
// store borrows - the borrowed-bytes contract the store's literals
// ride, so it must outlive the toast (a member, the same shape as any
// other toast source; a fresh failure may rewrite it, which only
// re-points the living toast's message). The Result crosses back
// either way, so the caller can WS_TRY the success value or echo the
// cancelled case. Cancelled is NOT a failure: it crosses back
// unadorned, no toast - the user asked for it.
template <class T>
ws::core::Result<T> toast_on_failure(Toasts& toasts, std::string& text,
                                      const ws::jobs::JobHandle<T>& handle,
                                      const char* what = "job") {
    ws::core::Result<T> result = handle.take();
    if (result.has_value()) {
        return result;
    }
    const ws::core::Error& error = result.error();
    if (error.code == static_cast<std::int32_t>(ws::jobs::JobError::cancelled)) {
        return result;
    }
    text = what;
    text += ": ";
    text += error.message;
    toasts.push({text.c_str(), ToastSeverity::error, nullptr, 4.0f});
    return result;
}

}  // namespace ws::ui::gui
