#include <ws/spice/parse.hpp>

#include <cctype>
#include <cstdlib>
#include <utility>

namespace ws::spice {

namespace {

constexpr const char* kMicron = "\xC2\xB5";  // µ, UTF-8

[[nodiscard]] bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
[[nodiscard]] bool is_ascii_letter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}
// Unit letters and other ignorable tails: ASCII letters, any UTF-8
// continuation/start bytes (µ included), but never digits, '.', '-'.
[[nodiscard]] bool is_ignorable_letter(unsigned char c) {
    return is_ascii_letter(static_cast<char>(c)) || c >= 0x80;
}

Diagnostic parse_diag(std::string message, SourceLoc loc = {}) {
    Diagnostic d;
    d.kind = ErrorKind::Parse;
    d.code = "spice.parse";
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

// --- one physical line -> tokens -------------------------------------------

struct Line {
    std::string_view text;
    int number;
};

// Strips an inline comment (';' anywhere; '$' when preceded by whitespace
// or leading the line) and trailing CR. Returns the comment-free view.
std::string_view strip_comment(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ';') {
            return line.substr(0, i);
        }
        if (c == '$' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            return line.substr(0, i);
        }
    }
    return line;
}

// Splits a comment-free line into whitespace-separated tokens with byte
// columns (1-based). '*' full-line comments are the caller's business.
void tokenize(std::string_view line, const std::string& file, int line_no,
              std::vector<Token>& out) {
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        Token t;
        t.text = std::string(line.substr(start, i - start));
        t.loc = SourceLoc::make(file, line_no, static_cast<int>(start) + 1);
        out.push_back(std::move(t));
    }
}

[[nodiscard]] bool first_token_is(const Record& r, std::string_view lower_key) {
    return !r.tokens.empty() && to_lower(r.tokens.front().text) == lower_key;
}

// --- deck reading -----------------------------------------------------------

struct Reader {
    const ParseOptions& opts;
    std::vector<std::string> active;  // include stack, cycle detection

    explicit Reader(const ParseOptions& o) : opts(o) {}

    // Processes one file's text; appends records to `deck.records`.
    // Returns false + filled error on failure; `ended` is set when '.end'
    // stops the whole deck.
    ws::core::Status process(const std::string& path, const std::string& text, Deck& deck,
                             bool& ended) {
        if (static_cast<int>(active.size()) > opts.max_include_depth) {
            return spice_fail(parse_diag(
                "include depth exceeds " + std::to_string(opts.max_include_depth)));
        }

        // Physical lines.
        std::vector<Line> lines;
        std::size_t pos = 0;
        int no = 0;
        while (pos <= text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t end = (nl == std::string::npos) ? text.size() : nl;
            // std::string::substr returns a string temporary; the Line
            // holds a VIEW, so take it through string_view (no dangling).
            lines.push_back(Line{std::string_view(text).substr(pos, end - pos), ++no});
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }

        Record current;  // the record under construction (continuations)
        bool have_current = false;

        auto flush = [&]() {
            if (have_current && !current.tokens.empty()) {
                deck.records.push_back(std::move(current));
            }
            current = Record{};
            have_current = false;
        };

        for (std::size_t li = 0; li < lines.size(); ++li) {
            const Line& raw = lines[li];

            // Line 1 is the title - ignored semantically, never a record
            // (only for the primary deck; an included file has no title).
            if (li == 0 && active.empty()) {
                deck.title = std::string(strip_comment(raw.text));
                continue;
            }

            const std::string_view line = strip_comment(raw.text);

            // Leading whitespace.
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                ++i;
            }
            if (i >= line.size()) {
                continue;  // blank
            }

            if (line[i] == '*') {
                continue;  // full-line comment
            }

            if (line[i] == '+') {
                // Continuation of the previous record.
                if (!have_current) {
                    return spice_fail(parse_diag(
                        "continuation '+' with nothing to continue",
                        SourceLoc::make(path, raw.number,
                                        static_cast<int>(i) + 1)));
                }
                tokenize(line.substr(i + 1), path, raw.number, current.tokens);
                continue;
            }

            flush();
            current.loc = SourceLoc::make(path, raw.number, static_cast<int>(i) + 1);
            have_current = true;
            tokenize(line.substr(i), path, raw.number, current.tokens);

            // '.end' terminates the whole deck (outer files too); it is a
            // terminator, not a record - discard it.
            if (first_token_is(current, ".end")) {
                current = Record{};
                have_current = false;
                ended = true;
                return {};
            }

            // '.include' expansion happens in place.
            if (first_token_is(current, ".include")) {
                SourceLoc where = current.loc;
                std::vector<Token> inc_tokens = std::move(current.tokens);
                have_current = false;
                flush();
                WS_TRY(include_file(path, std::move(inc_tokens), std::move(where), deck, ended));
                if (ended) {
                    return {};
                }
            }
        }
        flush();
        return {};
    }

    ws::core::Status include_file(const std::string& from_path, std::vector<Token> tokens,
                                  SourceLoc where, Deck& deck, bool& ended) {
        if (tokens.size() < 2) {
            Diagnostic d = parse_diag(".include needs a file name", std::move(where));
            d.code = "spice.parse.include_arity";
            return spice_fail(std::move(d));
        }
        if (tokens.size() > 2) {
            Diagnostic d = parse_diag(".include takes exactly one file name", std::move(where));
            d.code = "spice.parse.include_arity";
            return spice_fail(std::move(d));
        }
        if (!opts.include_resolver) {
            Diagnostic d = parse_diag(".include is unresolvable (no include resolver configured)",
                                      std::move(where));
            d.code = "spice.parse.include_unresolvable";
            return spice_fail(std::move(d));
        }

        const std::string target = tokens[1].text;  // case-exact (§8.1)

        // Candidate order: the including file's directory, then -I dirs.
        std::vector<std::string> candidates;
        const std::size_t slash = from_path.find_last_of("/\\");
        const std::string base_dir =
            (slash == std::string::npos) ? std::string() : from_path.substr(0, slash);
        if (!base_dir.empty()) {
            candidates.push_back(base_dir + "/" + target);
        }
        candidates.push_back(target);
        for (const std::string& dir : opts.include_dirs) {
            candidates.push_back(dir + "/" + target);
        }

        for (const std::string& candidate : candidates) {
            // Cycle: the candidate already on the active stack.
            for (const std::string& a : active) {
                if (a == candidate) {
                    Diagnostic d = parse_diag("include cycle through '" + candidate + "'",
                                              std::move(where));
                    d.code = "spice.parse.include_cycle";
                    d.names.push_back(candidate);
                    return spice_fail(std::move(d));
                }
            }
            ws::core::Result<std::string> text = opts.include_resolver(candidate);
            if (!text) {
                continue;  // try the next search path
            }
            active.push_back(candidate);
            ws::core::Status st = process(candidate, std::move(*text), deck, ended);
            active.pop_back();
            return st;
        }

        Diagnostic d = parse_diag("cannot resolve .include '" + target + "'", std::move(where));
        d.code = "spice.parse.include_missing";
        d.hint = "searched the including file's directory, then the -I paths";
        return spice_fail(std::move(d));
    }
};

}  // namespace

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

ws::core::Result<Deck> read_deck(std::string deck_path, std::string deck_text,
                                 const ParseOptions& opts) {
    Deck deck;
    bool ended = false;
    Reader reader(opts);
    WS_TRY(reader.process(deck_path, std::move(deck_text), deck, ended));
    return deck;
}

// --- SPICE numbers (§8.2) ----------------------------------------------------

namespace {

struct SuffixMatch {
    std::size_t length = 0;
    double factor = 0.0;
};

// Longest-match, case-insensitive, MEG/MIL before M (the scan order the
// unit tests pin). `rest` is the unparsed tail starting at the suffix.
SuffixMatch match_suffix(std::string_view rest) {
    const std::string lower = to_lower(rest);
    // 3-char suffixes first.
    if (lower.size() >= 3) {
        if (lower.substr(0, 3) == "meg") {
            return {3, 1e6};
        }
        if (lower.substr(0, 3) == "mil") {
            return {3, 25.4e-6};
        }
    }
    if (!lower.empty()) {
        switch (lower[0]) {
            case 't': return {1, 1e12};
            case 'g': return {1, 1e9};
            case 'k': return {1, 1e3};
            case 'm': return {1, 1e-3};
            case 'u': return {1, 1e-6};
            case 'n': return {1, 1e-9};
            case 'p': return {1, 1e-12};
            case 'f': return {1, 1e-15};
            default: break;
        }
    }
    // µ (two UTF-8 bytes) reads as u.
    if (rest.substr(0, 2) == std::string_view(kMicron)) {
        return {2, 1e-6};
    }
    return {0, 0.0};
}

ws::core::Result<double> number_error(std::string_view text, std::string why) {
    Diagnostic d;
    d.kind = ErrorKind::Parse;
    d.code = "spice.parse.number";
    d.message = "malformed number '" + std::string(text) + "': " + std::move(why);
    return spice_fail(std::move(d));
}

}  // namespace

ws::core::Result<double> parse_number(std::string_view text) {
    if (text.empty()) {
        return number_error(text, "empty");
    }
    std::size_t i = 0;
    const std::size_t n = text.size();
    if (text[i] == '+' || text[i] == '-') {
        ++i;  // the sign is part of the strtod prefix below
    }

    // Digits [ '.' digits ] - at least one digit overall ('.5' and '5.'
    // both legal).
    const std::size_t int_start = i;
    while (i < n && is_digit(text[i])) {
        ++i;
    }
    const bool int_digits = i > int_start;
    bool frac_digits = false;
    if (i < n && text[i] == '.') {
        ++i;
        const std::size_t frac_start = i;
        while (i < n && is_digit(text[i])) {
            ++i;
        }
        frac_digits = i > frac_start;
    }
    if (!int_digits && !frac_digits) {
        return number_error(text, "no digits");
    }

    // Optional exponent; a bare 'e' without digits folds back (it is a
    // unit letter instead).
    bool has_exponent = false;
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < n && (text[j] == '+' || text[j] == '-')) {
            ++j;
        }
        const std::size_t exp_start = j;
        while (j < n && is_digit(text[j])) {
            ++j;
        }
        if (j > exp_start) {
            i = j;
            has_exponent = true;
        }
    }

    // strtod over exactly the numeric prefix - the SIGN IS INCLUDED in the
    // prefix, so no manual negation anywhere (locale is never set away
    // from "C" in this library, so '.' is the decimal point).
    const std::string numeric(text.substr(0, i));
    char* end_ptr = nullptr;
    const double value = std::strtod(numeric.c_str(), &end_ptr);
    if (end_ptr != numeric.c_str() + numeric.size()) {
        return number_error(text, "internal: numeric prefix did not parse");
    }

    // Scale suffix, then trailing unit letters.
    double scaled = value;
    const std::string_view rest = text.substr(i);
    if (!rest.empty()) {
        const SuffixMatch suffix = match_suffix(rest);
        if (suffix.length > 0) {
            if (has_exponent) {
                return number_error(text, "a scale suffix cannot follow an exponent");
            }
            scaled = value * suffix.factor;
            i += suffix.length;
        }
        // Trailing letters (units) are ignored: '1uF', '10kOhm', '5V'.
        while (i < n && is_ignorable_letter(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i != n) {
            return number_error(text, "trailing '" + std::string(text.substr(i)) +
                                          "' is not a unit tail");
        }
    }

    return scaled;
}

ws::core::Result<double> parse_number(const Token& token) {
    ws::core::Result<double> v = parse_number(token.text);
    if (!v) {
        // Re-render with the token's location attached.
        ws::core::Error e = v.error();
        Diagnostic d;
        d.kind = ErrorKind::Parse;
        d.code = "spice.parse.number";
        d.message = "malformed number '" + token.text + "'";
        d.loc = token.loc;
        return ws::core::unexpected<ws::core::Error>(make_error(std::move(d)));
    }
    return v;
}

}  // namespace ws::spice
