#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/circsim/diag.hpp>
#include <ws/circsim/symbols.hpp>
#include <ws/settings/json.hpp>

// The .csym symbol-library format + the effective symbol table (apps/
// circsim stage 2). A .csym file is JSON:
//
//   {"format": "circsim-symbols", "version": 1,
//    "symbols": [
//      {"key": "tline", "name": "Lossless transmission line",
//       "spice": "T", "ref": "T",
//       "pins":  [{"name": "a1", "pos": [-4, 1], "dir": 180}, ...],
//       "value": false, "model": false,
//       "emit":  [{"param": "z0", "prefix": "Z0="}, ...],
//       "params":[{"key": "z0", "type": "number", "default": "50"}, ...],
//       "draw":  [{"line": [[-4, 1], [-1, 1]]}, ...]}
//    ]}
//
// Field defaults mirror the SymbolDef struct: "value" defaults true,
// "model" false, "spice"/"ref"/"fixed_net" absent = none. Pins are SPICE
// order = array order. Unknown fields at the ROOT and SYMBOL level ride
// in `unknown` and re-emerge on save (the .csim carrier philosophy);
// unknown fields inside the tight sub-objects (pins/emit/params/draw)
// are dropped WITH a warning - a typo there is something the author
// wants to hear about, not silence.
//
// Layering: a SymbolLibrary starts from the built-in table; every
// load_text layers its symbols on top, key case-insensitive, LATER
// WINS (redefining a key - over a built-in or an earlier file - is the
// point: that is how the shipped symbols get customized). Every
// redefinition reports a warning naming what it replaced.
//
// Load is tolerant in the .csim sense: wrong-typed fields repair to
// defaults, malformed symbols are skipped - all as Diag warnings (the
// missing "key" case is an error-severity note; the symbol still drops);
// only "not JSON at all" and "wrong format value" fail the load.

namespace ws::circsim {

inline constexpr std::string_view kCsymLibFormat = "circsim-symbols";
inline constexpr int kCsymLibVersion = 1;

class SymbolLibrary {
public:
    // Seeded with the full built-in table (specs + draw included).
    SymbolLibrary();

    // Case-insensitive lookup; null when unknown.
    [[nodiscard]] const SymbolDef* find(std::string_view key) const;

    // The effective table in definition order: built-ins first, then
    // library symbols in load order (an override keeps its position).
    [[nodiscard]] const std::vector<SymbolDef>& symbols() const {
        return symbols_;
    }

    // How many of the leading entries are the built-ins (save's cut).
    [[nodiscard]] std::size_t builtin_count() const { return builtin_count_; }

    // Layers one file's text on top. `origin` names the source in the
    // diagnostics ("" = anonymous). Hard failures (unparseable JSON,
    // wrong "format") return the error; everything recoverable rides
    // `diags`.
    ws::core::Status load_text(std::string_view text,
                               std::string_view origin,
                               std::vector<Diag>& diags);

    // The canonical serialization: format + version, the root's unknown
    // fields, then every symbol in table order with all known fields
    // (then its unknowns). `include_builtin` keeps the built-in prefix
    // (the full-table dump); the default round-trips a user library.
    [[nodiscard]] std::string save(bool include_builtin = false) const;

    // Root-level unknown fields from the loaded files (merged; a later
    // file's key overrides in place, first position kept).
    ws::settings::json::Object unknown;

private:
    // Rebuilds the ci lookup index from symbols_ (constructor use; the
    // load path maintains it incrementally).
    void reindex();

    std::vector<SymbolDef> symbols_;
    std::vector<std::uint8_t> source_;  // 0 built-in, 1 library-defined
    // lowercased key -> index into symbols_: find()'s one-lookup answer
    // (built at insert time so a lookup costs one lower, not a scan
    // that lowercases the whole table per comparison).
    std::map<std::string, std::size_t> by_key_;
    std::size_t builtin_count_ = 0;
};

// The process-wide default: the built-in table, nothing layered. The
// null-pointer defaults on build_nets/run_erc/export_deck/run_schematic
// resolve here.
[[nodiscard]] const SymbolLibrary& builtin_library();

}  // namespace ws::circsim
