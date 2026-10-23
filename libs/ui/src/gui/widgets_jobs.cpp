// The ws::jobs bridge's out-of-line half (nextplan stage 8): the
// mirror's store and the state-to-view mapping. The row and toast
// templates stay in the header with their JobHandle<T> parameter;
// everything with a fixed shape lives here, one more widget family in
// the src/gui archive.

#include <ws/ui/gui/job_progress.hpp>

#include <algorithm>

namespace ws::ui::gui {

void JobProgress::report(float fraction, const char* message) {
    // The record's clamp, applied again at the mirror: a hand-filled
    // mirror (the tests script it directly) cannot draw past the end
    // either. A null message is "no message this time" - cleared, not
    // kept: the latest report wins, all of it.
    fraction_ = std::clamp(fraction, 0.0f, 1.0f);
    if (message != nullptr) {
        message_ = message;
    } else {
        message_.clear();
    }
}

void JobProgress::reset() {
    fraction_ = 0.0f;
    message_.clear();
}

JobView job_view(ws::jobs::JobState state, const JobProgress& mirror,
                 const ChromeTheme& theme) {
    switch (state) {
    case ws::jobs::JobState::queued:
        // Queued wears the accent (the de-facto "active" face) with
        // whatever the previous run of the slot reported; the cancel
        // is live - a queued job cancels at pickup and never runs.
        return {mirror.fraction(), theme.control_pressed, "queued", true};
    case ws::jobs::JobState::running:
        // The live word is the mirror's latest message; the bare
        // "running" stands until the first report lands.
        return {mirror.fraction(), theme.control_pressed,
                mirror.message().empty() ? "running"
                                         : mirror.message().c_str(),
                true};
    case ws::jobs::JobState::done:
        return {1.0f, theme.control_on, "done", false};
    case ws::jobs::JobState::failed:
        // The toast-error red: the row and the toast it posts (via
        // toast_on_failure) read as one story.
        return {1.0f, theme.toast_error, "failed", false};
    case ws::jobs::JobState::cancelled:
        // The fraction stands as the mirror left it - a half-run job
        // stays visibly half-run - but the dead face: it is over.
        return {mirror.fraction(), theme.control_disabled, "cancelled",
                false};
    case ws::jobs::JobState::invalid:
    default:
        // The null handle: the idle row. Fill 0x00000000 draws nothing
        // inside the track (alpha 0), which is the point.
        return {0.0f, 0u, "no job", false};
    }
}

}  // namespace ws::ui::gui
