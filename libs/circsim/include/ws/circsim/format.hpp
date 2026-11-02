#pragma once

#include <string>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/circsim/diag.hpp>
#include <ws/circsim/model.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/json.hpp>

// The .csim file format (apps/circsim stage 1): JSON, "format":"circsim"
// plus "version", arrays sorted by id for stable diffs, unknown fields
// preserved verbatim on load/save (the insertion-order json::Object is the
// carrier - a field this build does not know rides through untouched, and
// a later build that learns it migrates it into the model).
//
// Load is tolerant: duplicate ids, dangling references and wrong-typed
// fields become diagnostics in the LoadReport (first-wins repairs) rather
// than failures; only "not JSON at all" and "not a circsim file" fail.
// Saves go through the settings FileSystem seam, whose write is atomic by
// contract (sibling .tmp + rename).

namespace ws::circsim {

inline constexpr int kCsimVersion = 1;
inline constexpr std::string_view kCsimFormat = "circsim";

struct LoadReport {
    std::vector<Diag> diags;
    int loaded_version = 0;  // what the file said (0 = absent -> migrated)
    bool migrated = false;   // a migration step ran
    bool future = false;     // the file claims a newer version than us

    [[nodiscard]] bool has_errors() const { return any_errors(diags); }
};

// Parses and loads. Hard failures (unparseable JSON, wrong "format" value)
// return the error; everything recoverable lands in `report`.
[[nodiscard]] ws::core::Result<Schematic> load_csim(std::string_view text,
                                                    LoadReport& report);

// Serializes: known fields in canonical order (arrays ascending by id),
// then each object's unknown fields in their original positions.
[[nodiscard]] std::string save_csim(const Schematic& s);

// The disk pair, through the injected FileSystem seam.
[[nodiscard]] ws::core::Result<Schematic> read_csim(
    const ws::settings::FileSystem& fs, std::string_view path,
    LoadReport& report);
[[nodiscard]] ws::core::Status write_csim(ws::settings::FileSystem& fs,
                                          std::string_view path,
                                          const Schematic& s);

}  // namespace ws::circsim
