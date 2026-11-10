#pragma once

#include <string>
#include <string_view>
#include <vector>

// The built-in model library (stage 25): the curated .model / .subckt
// cards a part can name without hand-writing anything. A semiconductor
// part's `model` param is just a NAME; when that name is one of these
// cards, export_deck appends the card's text to the deck after the
// directives block (the user's own card with the same name wins - the
// model table is a first-match scan, the subckt map an emplace), and
// the property grid offers the kind's cards as a combo. Pure data, the
// symbols-table's own shape: every card sticks to the parameter sets
// ws::spice's devices actually implement (the accepted-but-ignored
// vendor tail is deliberately absent - a library card that parses
// wrong is worse than none), and the opamps are linear macromodels
// (E/R/C subckts: differential Ri, the GBW pole, Avol, Ro) because a
// linear stage needs no .model of its own inside the subckt body.
//
// kinds map 1:1 onto the model TYPE tokens for .model cards ("d",
// "npn", "pnp", "nmos", "pmos", "njf", "pjf", "sw"); "opamp" is the
// one extension (a .subckt card, five ports out inp inn vp vm). A
// SymbolDef carries its kind in model_kind; ERC compares the two.

namespace ws::circsim {

struct ModelCard {
    std::string name;   // "1n4148" - the token a part's model param names (ci)
    std::string title;  // "1N4148" - the display spelling
    std::string kind;   // "d" | "npn" | "pnp" | "nmos" | "pmos" | "njf"
                        // | "pjf" | "sw" | "opamp" (lowercase)
    std::string text;   // the deck lines: one ".model name TYPE(...)"
                        // line, or a ".subckt ... .ends" block
    std::string desc;   // one-line human description (the picker's hint)
};

// The table, shelf order: diodes, BJTs, MOSFETs, JFETs, switches,
// opamps. Stable - callers may keep indices across calls.
[[nodiscard]] const std::vector<ModelCard>& builtin_models();

// Case-insensitive lookup by name; null when unknown.
[[nodiscard]] const ModelCard* find_builtin_model(std::string_view name);

// The cards of one kind (ci), table order; empty when the kind is
// unknown or carries nothing yet.
[[nodiscard]] std::vector<const ModelCard*> builtin_models_of_kind(
    std::string_view kind);

// The kind's display spelling for diagnostics ("d" -> "diode", "npn"
// -> "NPN transistor", ...); "model" for an unknown kind.
[[nodiscard]] std::string model_kind_label(std::string_view kind);

}  // namespace ws::circsim
