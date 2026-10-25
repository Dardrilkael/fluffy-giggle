#pragma once

#include <string>
#include <string_view>

namespace ws::platform {

// The lexical path helpers (stage 12). Canonical separators are '/'
// everywhere - '\' folds into it - and a path is an optional root
// (a drive root "C:/", a unc root "//" or a plain root "/") plus
// '/'-joined segments. Pure lexical work: no disk, no allocation
// beyond the returned strings.

// a + "/" + b, the concatenation normalized. An absolute right side
// wins outright (path resolution semantics); an empty side returns
// the other.
[[nodiscard]] std::string join_path(std::string_view a, std::string_view b);

// Collapse duplicate separators (either kind), drop "." segments and
// trailing separators, resolve ".." against earlier segments - never
// past the root. A relative ".." with nothing to pop stays ("a/../.."
// is ".."). Case is preserved, never folded.
[[nodiscard]] std::string normalize_path(std::string_view path);

// True for a plain root ("/a"), a unc root ("//a") or a drive root
// ("C:/a"). A drive without its root separator ("C:a") is relative.
[[nodiscard]] bool is_absolute_path(std::string_view path);

// The last segment, "" for a root or an empty path ("a/b/" is "b").
[[nodiscard]] std::string name_of(std::string_view path);

// Everything before the last segment, the root keeping its shape
// ("/a" is "/", "C:/a" is "C:/"); "" when there is no parent.
[[nodiscard]] std::string parent_of(std::string_view path);

}  // namespace ws::platform
