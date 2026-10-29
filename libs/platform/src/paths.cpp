#include <ws/platform/paths.hpp>

#include <ws/platform/errors.hpp>

#include <filesystem>
#include <system_error>
#include <vector>

namespace ws::platform {
namespace {

// utf-8 narrow -> fs::path. Routed through char8_t so non-ascii
// paths survive Windows too (the StdFileSystem spelling - one routing
// rule across the workspace's file seams).
[[nodiscard]] std::filesystem::path to_fs_path(std::string_view path) {
    return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

[[nodiscard]] bool is_sep(char c) { return c == '/' || c == '\\'; }

[[nodiscard]] bool is_drive_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace

std::string join_path(std::string_view a, std::string_view b) {
    if (b.empty()) {
        return normalize_path(a);
    }
    // A rooted right side replaces the left outright - same rule the
    // OS path resolvers apply to "cd a; cd /b".
    if (is_absolute_path(b)) {
        return normalize_path(b);
    }
    std::string left = normalize_path(a);
    if (left.empty()) {
        return normalize_path(b);
    }
    // A bare root ("/", "//", "C:/") already ends in its separator -
    // adding one would turn a plain root into a unc root.
    const bool left_is_root =
        left == "/" || left == "//" ||
        (left.size() == 3 && left[1] == ':' && is_sep(left[2]));
    std::string joined = left_is_root ? left + std::string{b}
                                      : left + "/" + std::string{b};
    return normalize_path(joined);
}

std::string normalize_path(std::string_view path) {
    std::string out;
    out.reserve(path.size());

    // Optional drive prefix, kept verbatim ("C:x" stays drive-relative).
    size_t i = 0;
    if (path.size() >= 2 && path[1] == ':' && is_drive_letter(path[0])) {
        out += path[0];
        out += ':';
        i = 2;
    }

    // The root separator run: unc keeps "//", a single separator is
    // "/", none means relative.
    size_t run = 0;
    while (i < path.size() && is_sep(path[i])) {
        ++run;
        ++i;
    }
    if (run >= 2) {
        out += "//";
    } else if (run == 1) {
        out += '/';
    }
    const bool rooted = run > 0;

    std::vector<std::string_view> segments;
    while (i < path.size()) {
        if (is_sep(path[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < path.size() && !is_sep(path[i])) {
            ++i;
        }
        const std::string_view segment = path.substr(start, i - start);
        if (segment == ".") {
            continue;
        }
        if (segment == "..") {
            // Never climb past the root; a relative climb with nothing
            // to pop stays a literal "..".
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!rooted) {
                segments.push_back(segment);
            }
            continue;
        }
        segments.push_back(segment);
    }

    // The first segment joins without a separator: a rooted path
    // already carries its "/" ("//", "C:/"), and a drive-only prefix
    // stays drive-relative ("C:x", never "C:/x").
    bool first = true;
    for (const std::string_view segment : segments) {
        if (!first) {
            out += '/';
        }
        out += segment;
        first = false;
    }
    // Rooted and empty ("/", "C:/", "//") keeps its bare root; a
    // relative path that normalizes to nothing is the empty string.
    return out;
}

bool is_absolute_path(std::string_view path) {
    if (!path.empty() && is_sep(path[0])) {
        return true;
    }
    return path.size() >= 3 && path[1] == ':' && is_drive_letter(path[0]) &&
           is_sep(path[2]);
}

std::string name_of(std::string_view path) {
    size_t end = path.size();
    while (end > 0 && is_sep(path[end - 1])) {
        --end;
    }
    size_t start = end;
    while (start > 0 && !is_sep(path[start - 1])) {
        --start;
    }
    return std::string{path.substr(start, end - start)};
}

std::string parent_of(std::string_view path) {
    size_t end = path.size();
    while (end > 0 && is_sep(path[end - 1])) {
        --end;
    }
    size_t start = end;
    while (start > 0 && !is_sep(path[start - 1])) {
        --start;
    }
    if (start == 0) {
        return {};  // no separator: no parent ("a", "", "/")
    }
    const std::string_view head = path.substr(0, start);
    size_t cut = head.size();
    while (cut > 0 && is_sep(head[cut - 1])) {
        --cut;
    }
    // A root keeps its shape: the drive root's separator IS the root
    // ("C:/a"), and a separator-only head is "/" or "//".
    if (cut == 2 && head[1] == ':' && is_drive_letter(head[0])) {
        std::string out{head.substr(0, 2)};
        out += '/';
        return out;
    }
    if (cut == 0) {
        return std::string{head.substr(0, head.size() >= 2 && is_sep(head[1]) ? 2 : 1)};
    }
    return std::string{head.substr(0, cut)};
}

// The mkdir -p over utf-8 narrow paths: a pure std::filesystem utility
// (moved here from locations.cpp so the headless consumers - the
// autosave store, the recent list - link without the OS location half).
ws::core::Status ensure_directory(std::string_view path) {
    if (path.empty()) {
        return fail(PlatformError::invalid_argument,
                    "ensure_directory: the path is empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(to_fs_path(path), ec);
    if (ec) {
        return fail(PlatformError::io_failed,
                    "mkdir '" + std::string{path} + "': " + ec.message());
    }
    return {};
}

}  // namespace ws::platform
