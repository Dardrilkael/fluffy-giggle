// The single-instance grab: one OS-wide lock per key, held for the
// process lifetime, plus the open-with argv parse. The second
// instance is expected to show its complaint and exit - forwarding
// its documents to the primary arrives with the first app that
// needs the channel (a documented cut, not a forgotten one). The
// key must be non-empty, a caller contract debug-caught; an empty
// key grabs nothing and answers primary.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ws::platform {

// Holds the grab for the process lifetime. Move-only: the lock
// outlives any one reference to it.
class SingleInstance {
  public:
    // Grabs immediately. On android the OS owns single-instancing
    // (the manifest's launchMode) - every process is primary.
    explicit SingleInstance(std::string_view key);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&& other) noexcept;
    SingleInstance& operator=(SingleInstance&& other) noexcept;

    // False in a second process (the usual exit check) and in a
    // second same-key grab inside this one.
    [[nodiscard]] bool is_primary() const noexcept;

  private:
    void* handle_ = nullptr;  // the OS lock, opaque
    bool primary_ = false;
};

// The open-with hook: the document paths from a command line.
// argv[0] (the program) and switches are skipped - the Windows
// switch spellings "-" and "/" - and the rest come back verbatim:
// resolving is the caller's. A "/"-rooted POSIX path would read as
// a switch; the workspace's desktop is Windows-only.
[[nodiscard]] inline std::vector<std::string> open_documents(
    int argc, const char* const* argv) {
    std::vector<std::string> documents;
    for (int i = 1; i < argc; ++i) {
        const char* const raw = argv[i];
        if (raw == nullptr) {
            continue;
        }
        const std::string_view arg = raw;
        if (arg.empty() || arg.front() == '-' || arg.front() == '/') {
            continue;
        }
        documents.emplace_back(arg);
    }
    return documents;
}

}  // namespace ws::platform
