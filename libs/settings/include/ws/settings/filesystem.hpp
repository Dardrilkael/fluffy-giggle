#pragma once

#include <string>
#include <string_view>

#include <ws/core/result.hpp>

namespace ws::settings {

// The minimal file seam the store's persistence rides on - and the
// seam later stages reuse (theme loading, platform services), since
// the workspace has no shared fs abstraction. Five verbs, nothing
// more: read, an ATOMIC write, rename, remove, exists. Tests inject
// a fake; production rides StdFileSystem.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // The file's whole content. A missing file answers io_failed -
    // the caller decides whether that is a first run or trouble.
    [[nodiscard]] virtual ws::core::Result<std::string> read(
        std::string_view path) const = 0;

    // Write the whole file ATOMICALLY: the bytes land in a sibling
    // ".tmp" first, then one rename replaces the target - so a
    // failure anywhere answers io_failed and leaves the previous
    // file (if any) untouched. The temp name is fixed: one path has
    // one writer (the store's owner thread).
    virtual ws::core::Status write(std::string_view path,
                                   std::string_view bytes) = 0;

    // Replace `to` with `from`. An existing `to` is overwritten.
    virtual ws::core::Status rename(std::string_view from,
                                    std::string_view to) = 0;

    // True when the file is gone afterwards - removed, or never
    // there; false when it exists and could not be removed.
    virtual bool remove(std::string_view path) = 0;

    // False when the answer cannot be determined (a missing parent,
    // a permission wall).
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
};

// The std::filesystem-backed default. Paths are utf-8 narrow strings
// on every platform (routed through char8_t, so non-ascii paths
// survive Windows too). Every failure answers io_failed naming the
// verb and the path.
class StdFileSystem final : public FileSystem {
public:
    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override;
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override;
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override;
    bool remove(std::string_view path) override;
    [[nodiscard]] bool exists(std::string_view path) const override;
};

}  // namespace ws::settings
