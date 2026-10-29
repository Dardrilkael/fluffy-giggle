#pragma once

// Internal (src-only): one record -> one typed card. Shared by
// build_deck_ast (top level) and elaboration (subckt bodies, re-validated
// per instance). Not part of the public include/ surface.

#include <map>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>

namespace ws::spice::cards {

struct ParsedCard {
    enum class Kind {
        Device,
        Param,
        Model,
        Options,
        Op,
        Dc,
        Subckt,  // ".subckt" opener - only meaningful at deck level
        Ends,    // ".ends" - ditto
    };
    Kind kind = Kind::Device;

    DeviceCard device;
    std::vector<ParamDef> params;  // Kind::Param: one per name=value on the card
    ModelDef model;
    OptionsCard options;
    OpCard op;
    DcCard dc;
    SourceLoc loc;

    // Device letter, uppercased ('R', 'V', 'X', ...), valid for Kind::Device.
    char letter = '\0';
};

// Validates and parses one record. `inside_subckt` rejects analysis and
// structural cards (.op/.dc/.subckt) where they cannot mean anything.
[[nodiscard]] ws::core::Result<ParsedCard> parse_card(const Record& rec, bool inside_subckt,
                                                      std::vector<Warning>& warnings);

// Device letters by milestone (for the not-yet messages).
[[nodiscard]] const char* unsupported_device_milestone(char letter);

// The builtin M1 factory table ('R','V','I','E','F','G','H'), defined in
// devices.cpp. The elaborator copies it into its own table (§3.4) and
// rejects letters not present with a line-numbered error.
const std::map<char, DeviceFactory>& builtin_device_factories();

}  // namespace ws::spice::cards
