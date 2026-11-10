#pragma once

// The circsim-wide string utilities (private to the library's TUs).
// One definition each: these used to live as per-file copies in erc,
// export, nets and locate, drifting in small ways - the lowercase one
// being locale-dependent (std::tolower answers to LC_CTYPE), which is
// exactly the bug class this header exists to close.

#include <string>
#include <string_view>

namespace ws::circsim {

// ASCII-only lowercase: 'A'-'Z' -> 'a'-'z', everything else untouched.
// Locale-independent by construction (std::tolower answers to LC_CTYPE;
// refs, symbol keys and net names are ASCII identifiers). Deterministic
// on every platform, every locale.
[[nodiscard]] std::string lower(std::string_view s);

// A safe SPICE identifier: non-empty, ASCII letters/digits/underscore
// only (ERC's ref rule and the export's element-name grammar).
[[nodiscard]] bool is_identifier(std::string_view s);

}  // namespace ws::circsim
