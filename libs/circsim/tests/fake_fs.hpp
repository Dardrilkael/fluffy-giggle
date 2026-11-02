#pragma once

// The in-memory FileSystem for the .csim format tests (the settings seam's
// fake-disk pattern).

#include <map>
#include <string>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::circsim::testing {

class FakeFs final : public ws::settings::FileSystem {
public:
    std::map<std::string, std::string> files;
    int writes = 0;

    ws::core::Result<std::string> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) {
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "settings", "no such file '" + std::string(path) + "'"));
        }
        return it->second;
    }
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        files[std::string(path)] = std::string(bytes);
        ++writes;
        return {};
    }
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto it = files.find(std::string(from));
        if (it == files.end()) {
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "settings", "no such file '" + std::string(from) + "'"));
        }
        files[std::string(to)] = std::move(it->second);
        files.erase(it);
        return {};
    }
    bool remove(std::string_view path) override {
        return files.erase(std::string(path)) > 0;
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
};

}  // namespace ws::circsim::testing
