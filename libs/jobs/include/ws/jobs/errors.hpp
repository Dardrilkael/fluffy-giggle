#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::jobs {

// ws::jobs's error codes (root CLAUDE.md, "Error handling"): every public
// failure returns a ws::core::Error with category "jobs" and one of these as
// its code. The enum stays ws::jobs's - core never knows these values. Codes
// are append-only: never renumber, only add.
enum class JobError : std::int32_t {
    invalid_argument = 1,  // malformed argument (null handle where work was expected, ...)
    cancelled = 2,         // the job was cancelled - the honoured-cancel return
    not_finished = 3,     // take() on a job whose result is not terminal yet
    already_taken = 4,     // take() moved the Result away on an earlier call
    shutting_down = 5,     // the system is tearing down; submit() refused
    exception = 6,         // a job threw; the message carries what()
    invalid_handle = 7,    // null/never-issued job handle used as if live
};

// One-line description per code, from a static table. This is the ONLY
// message surface inside callbacks (the workspace rule: callbacks never
// allocate to report errors): the string_view points at this table, so it
// stays valid forever - nothing builds messages per call.
[[nodiscard]] constexpr std::string_view describe(JobError code) noexcept {
    switch (code) {
        case JobError::invalid_argument: return "invalid argument";
        case JobError::cancelled: return "cancelled";
        case JobError::not_finished: return "not finished";
        case JobError::already_taken: return "already taken";
        case JobError::shutting_down: return "shutting down";
        case JobError::exception: return "job threw an exception";
        case JobError::invalid_handle: return "invalid handle";
    }
    return "unknown jobs error";
}

[[nodiscard]] inline ws::core::Error make_job_error(JobError code,
                                                    std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "jobs",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(JobError::cancelled);
// RESULT-PATH ONLY: this allocates (ws::core::Error::message is a
// std::string by design). Callback surfaces carry JobError + describe()
// instead, never a dynamically built message. The name hides ws::core::fail
// inside ws::jobs on purpose: jobs errors go through the enum, never raw
// codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    JobError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_job_error(code, std::move(message)));
}

}  // namespace ws::jobs
