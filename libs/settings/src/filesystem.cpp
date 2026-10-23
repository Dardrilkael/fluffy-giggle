#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <ws/settings/errors.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::settings {
namespace {

// utf-8 narrow string -> fs::path. Routed through char8_t so
// non-ascii paths survive Windows too (posix reads the same bytes as
// its native utf-8).
[[nodiscard]] std::filesystem::path to_path(std::string_view path) {
    return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

[[nodiscard]] ws::core::unexpected<ws::core::Error> io_error(
    std::string_view verb, std::string_view path, std::string detail) {
    return fail(SettingsError::io_failed,
                std::string{verb} + " '" + std::string{path} + "': " +
                    std::move(detail));
}

}  // namespace

ws::core::Result<std::string> StdFileSystem::read(
    std::string_view path) const {
    std::ifstream in{to_path(path), std::ios::binary};
    if (!in) {
        return io_error("read", path, "cannot open");
    }
    std::string out{std::istreambuf_iterator<char>{in},
                    std::istreambuf_iterator<char>{}};
    if (in.bad()) {
        return io_error("read", path, "read error");
    }
    return out;
}

ws::core::Status StdFileSystem::write(std::string_view path,
                                      std::string_view bytes) {
    const std::string temp_path = std::string{path} + ".tmp";
    {
        std::ofstream out{to_path(temp_path),
                          std::ios::binary | std::ios::trunc};
        if (!out) {
            return io_error("write", path, "cannot open the temp file");
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            (void)remove(temp_path);
            return io_error("write", path, "the temp write failed");
        }
    }  // closed before the rename: an open file cannot move on Windows
    std::error_code ec;
    std::filesystem::rename(to_path(temp_path), to_path(path), ec);
    if (ec) {
        (void)remove(temp_path);
        return io_error("write", path, "rename failed: " + ec.message());
    }
    return {};
}

ws::core::Status StdFileSystem::rename(std::string_view from,
                                       std::string_view to) {
    std::error_code ec;
    std::filesystem::rename(to_path(from), to_path(to), ec);
    if (ec) {
        return io_error("rename", from, ec.message());
    }
    return {};
}

bool StdFileSystem::remove(std::string_view path) {
    std::error_code ec;
    std::filesystem::remove(to_path(path), ec);
    return !ec;
}

bool StdFileSystem::exists(std::string_view path) const {
    std::error_code ec;
    return std::filesystem::exists(to_path(path), ec);
}

}  // namespace ws::settings
