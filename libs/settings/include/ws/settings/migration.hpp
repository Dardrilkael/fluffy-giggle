#pragma once

#include <cstdint>
#include <functional>
#include <map>

#include <ws/core/result.hpp>
#include <ws/settings/errors.hpp>
#include <ws/settings/json.hpp>

namespace ws::settings {

// One migration step: rewrite a parsed layer file from shape n to
// shape n+1, in place. A step may freely move, retype or drop keys -
// the unknown ones included - and answers Status. Steps run on the
// owner thread, once per load, and are registered once at startup;
// whatever context a step needs rides its own capture.
using MigrationStep = std::function<ws::core::Status(json::Value&)>;

// The versioned migration chain. Steps are registered by the shape
// they walk FROM (a step added at n rewrites n -> n+1); run() walks a
// document's declared version step by step up to the schema's current
// version, then stamps the document with it.
class Migrations {
public:
    Migrations() = default;

    // Register the step that walks shape `from` to shape `from + 1`.
    // A `from` below 1, a null step or a duplicate `from` answers
    // invalid_argument - an ambiguous chain is a programmer error.
    ws::core::Status add(std::int64_t from, MigrationStep step);

    // Walk `document` to `current`. The root must be an object; a
    // missing "version" reads as 1 (the pre-versioning shape), a
    // non-integer one as corrupt. A document newer than `current`, or
    // a missing step anywhere in the walk, answers migrate_failed
    // naming the gap; a failing step's own Status is the walk's
    // Status. On success the document carries "version": current - a
    // step that rebuilt the root as a non-object answers corrupt_file.
    ws::core::Status run(json::Value& document, std::int64_t current) const;

private:
    std::map<std::int64_t, MigrationStep> steps_;
};

}  // namespace ws::settings
