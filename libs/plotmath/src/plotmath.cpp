// ws::plotmath - the implementation (see include/ws/plotmath/plotmath.hpp
// for the contracts). Pure <cmath>/<algorithm> + std; no font, no canvas,
// no ws::spice, no ui - the headless suite gates every decision the two
// plotting surfaces render.
//
// Provenance note: this TU is the merge of libs/circsim/src/plot.cpp's
// math and libs/ui/src/gui/widgets_plot.cpp's file-local copies of the
// same functions (which had themselves started as one and drifted - see
// the header). The circsim spelling is the body here wherever the two
// differed in guard set, because its guards are strictly a superset (the
// deep-zoom 2^53 refusal, the underflowed-span refusal, the exponent-form
// mantissa re-spelling); the widget's fixed 32-slot tick cap rides beside
// them (a huge target's visible refusal). fmt_num/parse_plain_number are
// inlined privately here so this lib depends on nothing but core-tier
// std - circsim's text.hpp keeps its own copies for its own callers.

#include "ws/plotmath/plotmath.hpp"

#include <algorithm>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ws::plotmath {

namespace {

// 10^e without std::pow's round-trip surprises on exact decades: the
// table below covers every exponent the prefix walk visits, so the
// multiply stays a single rounding.
[[nodiscard]] double pow10(int e) {
    static constexpr double kTable[] = {
        1e-15, 1e-12, 1e-9, 1e-6, 1e-3, 1e0,  1e3, 1e6,
        1e9,   1e12,  1e15,
    };
    constexpr int kBase = -15;
    constexpr int kStep = 3;
    static_assert(sizeof kTable / sizeof kTable[0] == 11,
                  "the f..T walk below indexes this table by (p - kBase) "
                  "in steps of kStep");
    const int at = e - kBase;
    if (at < 0 || at % kStep != 0) {
        return std::pow(10.0, static_cast<double>(e));
    }
    const int slot = at / kStep;
    if (slot < 0 || slot >= static_cast<int>(sizeof kTable /
                                              sizeof kTable[0])) {
        return std::pow(10.0, static_cast<double>(e));
    }
    return kTable[static_cast<std::size_t>(slot)];
}

// Locale-independent formatting into a caller buffer - std::to_chars
// with chars_format::general is guaranteed to use the '.' decimal
// separator regardless of the current LC_NUMERIC locale. The precision
// parameter matches the %.*g significant-digits semantics.
void fmt_num(char* buf, std::size_t cap, double v, int digits) {
    auto result = std::to_chars(buf, buf + cap, v,
                                std::chars_format::general, digits);
    if (result.ec == std::errc()) {
        *result.ptr = '\0';
    } else {
        // Fallback (should not happen with our buffer sizes): raw %g
        // in the C locale. This mirrors the old snprintf path but is
        // explicitly locale-pinned.
        const char* old_locale = std::setlocale(LC_NUMERIC, "C");
        std::snprintf(buf, cap, "%.*g", digits, v);
        std::setlocale(LC_NUMERIC, old_locale);
    }
}

// Parse the ASCII decimal fmt_num emits - the mantissa round-trip
// check. Floating-point std::from_chars is not implemented by every
// supported libc++ (notably the Android NDK toolchain), so this is a
// tiny hand parser: sign, digits, one dot, optional e/E exponent;
// refuses anything else. Returns false without touching `out`.
[[nodiscard]] bool parse_plain_number(const std::string& text,
                                       double& out) {
    const char* p = text.c_str();
    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = *p == '-';
        ++p;
    }
    double value = 0.0;
    int digits = 0;
    int fractional_digits = 0;
    bool fractional = false;
    while (*p != '\0') {
        if (*p == '.' && !fractional) {
            fractional = true;
            ++p;
            continue;
        }
        if (*p < '0' || *p > '9') {
            break;
        }
        value = value * 10.0 + static_cast<double>(*p - '0');
        ++digits;
        if (fractional) {
            ++fractional_digits;
        }
        ++p;
    }
    if (digits == 0) {
        return false;
    }
    int exponent = -fractional_digits;
    if (*p == 'e' || *p == 'E') {
        ++p;
        bool exponent_negative = false;
        if (*p == '-' || *p == '+') {
            exponent_negative = *p == '-';
            ++p;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        int parsed_exponent = 0;
        while (*p >= '0' && *p <= '9') {
            parsed_exponent = parsed_exponent * 10 + (*p - '0');
            ++p;
        }
        exponent += exponent_negative ? -parsed_exponent
                                      : parsed_exponent;
    }
    if (*p != '\0') {
        return false;
    }
    out = (negative ? -value : value) *
          std::pow(10.0, static_cast<double>(exponent));
    return std::isfinite(out);
}

// The min/max bucket walk both decimators share: `bucket_of(i)` maps a
// sample index to its bucket; each bucket keeps its y-min and y-max
// SAMPLE in occurrence order (so the polyline through the kept points
// keeps the segment's true slope), and the first and last samples are
// forced in - a waveform's edge timing rides them. A bucket that OPENS
// on a NaN keeps it (the finite samples behind it compare false against
// it), the same reading for both callers.
template <class BucketOf>
[[nodiscard]] std::vector<std::size_t> minmax_walk(
    std::span<const double> ys, std::size_t buckets, BucketOf bucket_of) {
    const std::size_t n = ys.size();
    std::vector<std::size_t> out;
    out.reserve(buckets * 2u + 2u);
    double min_v = ys[0];
    double max_v = ys[0];
    std::size_t min_i = 0;
    std::size_t max_i = 0;
    std::size_t open = 0;  // the bucket the tracked extrema belong to
    const auto flush = [&] {
        if (min_i == max_i) {
            // A bucket the sweep only grazed: one sample, one entry.
            out.push_back(min_i);
            return;
        }
        if (min_i < max_i) {
            out.push_back(min_i);
            out.push_back(max_i);
        } else {
            out.push_back(max_i);
            out.push_back(min_i);
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t b = bucket_of(i);
        if (b != open) {
            flush();
            min_v = max_v = ys[i];
            min_i = max_i = i;
            open = b;
            continue;
        }
        if (ys[i] < min_v) {
            min_v = ys[i];
            min_i = i;
        }
        if (ys[i] > max_v) {
            max_v = ys[i];
            max_i = i;
        }
    }
    flush();
    if (out.front() != 0u) {
        out.insert(out.begin(), 0u);
    }
    if (out.back() != n - 1u) {
        out.push_back(n - 1u);
    }
    return out;
}

}  // namespace

// The tick cap: a pathological huge target cannot overflow the walk -
// the answer stops at 32 ticks (both historical spellings' ceiling; the
// widget spelled it a fixed array, circsim relied on the caller's
// sanity. One number, one place).
constexpr std::size_t kTickCap = 32u;

std::vector<double> nice_ticks(double lo, double hi, unsigned target) {
    if (std::isnan(lo) || std::isnan(hi)) {
        return {};
    }
    if (lo > hi) {
        return {};
    }
    if (!(lo < hi)) {
        // Degenerate: one honest tick. (lo == hi == -inf lands here
        // too via the !(<) after the isnan guard; a single -inf tick
        // is still the truthful answer.)
        return {lo};
    }
    if (target == 0) {
        target = 1;
    }

    const double span = hi - lo;
    if (std::isinf(span)) {
        // An infinite span cannot walk a ladder; the caller's finite
        // bounds came from finite data, so this is a defensive branch.
        return {};
    }

    const double raw = span / static_cast<double>(target);
    if (!(raw > 0.0) || !std::isfinite(raw)) {
        return {};  // span underflowed against target: no ladder exists
    }
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    if (!(mag > 0.0) || !std::isfinite(mag)) {
        return {};  // denormal raw: 10^floor(log10) underflowed to 0
    }
    const double norm = raw / mag;
    double step = 10.0;
    if (norm < 1.5) {
        step = 1.0;
    } else if (norm < 3.0) {
        step = 2.0;
    } else if (norm < 7.0) {
        step = 5.0;
    }
    step *= mag;
    if (!(step > 0.0) || !std::isfinite(step)) {
        return {};
    }

    // First tick at or above lo (ceil on the grid), then every step
    // until past hi. The epsilon guard keeps a tick that IS hi from
    // falling to representable-noise; k*step from a fixed k0 keeps the
    // ticks exact multiples of step (no accumulation).
    const double eps = step * 1e-9;
    const double k0 = std::ceil((lo - eps) / step);
    const double k1 = std::floor((hi + eps) / step);
    std::vector<double> out;
    if (k1 < k0) {
        return out;
    }
    // k is walked by ++k: past 2^53 a double no longer resolves +1, so
    // the loop below would never reach k1 (a window narrower than the
    // ulp of its own offset, e.g. a deep zoom at a large x). The
    // ticks are unresolvable there - the honest answer is none.
    constexpr double kExact = 9007199254740992.0;  // 2^53
    if (!(std::fabs(k0) < kExact) || !(std::fabs(k1) < kExact)) {
        return {};
    }
    // The cap bounds the walk by construction (span/step is at most
    // ~1.4 x target for sane targets); a pathological target is the
    // caller's bug, and the capped answer makes it visible, not
    // fatal - the widget's own historical spelling of this refusal.
    out.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(k1 - k0) + 1u, kTickCap));
    for (double k = k0; k <= k1; ++k) {
        if (out.size() >= kTickCap) {
            break;
        }
        double t = k * step;
        if (t == 0.0) {
            t = 0.0;  // normalize -0.0 so labels print "0", not "-0"
        }
        out.push_back(t);
    }
    return out;
}

std::string format_eng(double v, int digits) {
    if (std::isnan(v)) {
        return "nan";
    }
    if (std::isinf(v)) {
        return v > 0.0 ? "inf" : "-inf";
    }
    if (v == 0.0) {
        return "0";
    }
    if (digits < 1) {
        digits = 1;
    }
    if (digits > 6) {
        digits = 6;
    }

    static constexpr char kPrefixes[] = {'f', 'p', 'n', 'u',
                                         'm', 0,   'k', 'M', 'G', 'T'};
    constexpr int kFirstExp = -15;  // femto
    constexpr int kLastExp = 12;    // tera
    constexpr int kStep = 3;

    const bool neg = v < 0.0;
    double av = neg ? -v : v;

    // Beyond the table both ways: plain %g keeps the magnitude honest
    // ("1e+18") where a clamped prefix ("1000T") would already be a
    // rounding story. Inside, two passes: a mantissa that rounds up
    // through its decade ("999.9m" at 4 digits) renormalizes to the
    // next prefix rather than printing 1000m.
    if (!(av >= 1e-15) || !(av < 1e15)) {
        char buf[32];
        fmt_num(buf, sizeof buf, v, digits);
        return buf;
    }
    // The initial bucket, clamped to the table before any
    // renormalization walks it upward.
    int e = static_cast<int>(std::floor(std::log10(av) / kStep) * kStep);
    if (e < kFirstExp) {
        e = kFirstExp;
    }
    if (e > kLastExp) {
        e = kLastExp;
    }
    for (int pass = 0; pass < 2; ++pass) {
        const double scaled = av / pow10(e);
        char mant_buf[32];
        fmt_num(mant_buf, sizeof mant_buf, scaled, digits);
        std::string mant = mant_buf;
        if (mant.find_first_of("eE") != std::string::npos) {
            // %g slid into exponent form ("2e+01" for 25 at 1 digit,
            // "1e+03" for 999.9 at 3): the mantissa would print as
            // "2e+01m", or fail the round-trip below and bump a
            // prefix it should not. Round to `digits` significant
            // figures here and spell the result out in plain digits.
            const double p = std::pow(
                10.0, std::floor(std::log10(scaled)) -
                          static_cast<double>(digits) + 1.0);
            const double rounded = std::round(scaled / p) * p;
            mant = rounded < 1000.0 ? [&] {
                char b[32];
                fmt_num(b, sizeof b, rounded, 3);
                return std::string(b);
            }()
                                    : std::string("1000");
        }
        // The round-trip check (the old std::atof, but honest about
        // what it consumed and deaf to LC_NUMERIC). A parse failure
        // cannot happen for our own output; if it ever did, treating
        // the mantissa as out-of-bucket walks the renormalization
        // instead of accepting garbage.
        const double printed =
            [&] {
                double parsed = 1000.0;
                if (!parse_plain_number(mant, parsed)) {
                    parsed = 1000.0;
                }
                return parsed;
            }();
        if (printed < 1000.0) {
            std::string out;
            if (neg && printed != 0.0) {
                out.push_back('-');
            }
            out += mant;
            const int slot = (e - kFirstExp) / kStep;
            if (kPrefixes[static_cast<std::size_t>(slot)] != 0) {
                out.push_back(kPrefixes[static_cast<std::size_t>(slot)]);
            }
            return out;
        }
        // The mantissa rounded up through its decade: renormalize the
        // BUCKET (e += 3), not the value - re-deriving e from log10 of
        // a rescaled av is off by one at exact decade boundaries
        // (0.9999 lands on the 999.99.. side of both neighbors).
        e += kStep;
        if (e > kLastExp) {
            // Rounding up past tera: beyond the table, plain %g.
            char buf[32];
            fmt_num(buf, sizeof buf, v, digits);
            return buf;
        }
    }
    // Unreachable for inputs inside the table (the renormalized value
    // re-buckets strictly below the ladder top); kept as the honest
    // raw %g tail a promising-string function owes its callers.
    char buf[32];
    fmt_num(buf, sizeof buf, v, digits);
    return buf;
}

const char* format_eng_buf(char* buf, std::size_t cap, double v,
                           int digits) {
    if (cap == 0u) {
        return buf;
    }
    // One walk, two spellings: the string answer is byte-identical to
    // the buffer one - the caller-scratch discipline (no allocation
    // mid-frame) over the same decisions.
    const std::string s = format_eng(v, digits);
    const std::size_t n = std::min(cap - 1u, s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return buf;
}

PlotRange expand_range(double lo, double hi, double frac) {
    if (std::isnan(lo) || std::isnan(hi) || std::isinf(lo) ||
        std::isinf(hi)) {
        return {0.0, 1.0};
    }
    if (frac < 0.0 || !std::isfinite(frac)) {
        frac = 0.0;
    }
    if (!(lo < hi)) {
        // Degenerate: pad by frac of the magnitude floor so a constant
        // line still gets a visible axis.
        const double d = std::max(std::fabs(lo), 1.0) * frac;
        return {lo - d, lo + d};
    }
    const double pad = (hi - lo) * frac;
    if (!std::isfinite(pad)) {
        return {lo, hi};  // span overflowed: padding cannot be honest
    }
    return {lo - pad, hi + pad};
}

// --- the manual view (see the header for the contracts) -----------------------

PlotRange clamp_view(PlotRange view, PlotRange home) {
    if (std::isnan(home.lo) || std::isnan(home.hi) || std::isinf(home.lo) ||
        std::isinf(home.hi) || !(home.lo < home.hi)) {
        return home;  // nothing sane to clamp against
    }
    if (std::isnan(view.lo) || std::isnan(view.hi) || std::isinf(view.lo) ||
        std::isinf(view.hi)) {
        return home;
    }
    // Degenerate, or a window at or past home's span (it cannot slide
    // inside; spilling both ways is the same case): the home fit wins.
    if (!(view.lo < view.hi) ||
        (view.hi - view.lo) >= (home.hi - home.lo)) {
        return home;
    }
    // Both ends inside and narrower than home: slide the minimal
    // distance. At most one shift fires (the span fits), but run both
    // so an already-clamped caller stays clamped.
    if (view.lo < home.lo) {
        const double d = home.lo - view.lo;
        view.lo += d;
        view.hi += d;
    }
    if (view.hi > home.hi) {
        const double d = view.hi - home.hi;
        view.lo -= d;
        view.hi -= d;
    }
    return view;
}

PlotRange zoom_range(PlotRange view, PlotRange home, double anchor,
                     double factor) {
    if (!(factor > 0.0) || std::isnan(factor) || factor == 1.0) {
        return view;
    }
    if (std::isnan(view.lo) || std::isnan(view.hi) || std::isinf(view.lo) ||
        std::isinf(view.hi) || !(view.lo < view.hi)) {
        return view;  // degenerate/non-finite: no direction to grow in
    }
    if (std::isnan(anchor)) {
        anchor = view.lo;
    } else {
        if (anchor < view.lo) {
            anchor = view.lo;
        }
        if (anchor > view.hi) {
            anchor = view.hi;
        }
    }
    const double k = 1.0 / factor;
    PlotRange out{anchor + (view.lo - anchor) * k,
                  anchor + (view.hi - anchor) * k};
    // The float-mush floor: a window narrower than 1e-6 of home's span
    // prints degenerate tick ladders and loses the data to rounding.
    if (std::isnan(out.lo) || std::isnan(out.hi) || std::isinf(out.lo) ||
        std::isinf(out.hi) || !(out.lo < out.hi) ||
        (out.hi - out.lo) < (home.hi - home.lo) * 1e-6) {
        const double span = (home.hi - home.lo) * 1e-6;
        out = {anchor - span * 0.5, anchor + span * 0.5};
    }
    return clamp_view(out, home);
}

PlotRange pan_range(PlotRange view, PlotRange home, double delta) {
    if (std::isnan(delta) || std::isinf(delta)) {
        return view;
    }
    return clamp_view({view.lo + delta, view.hi + delta}, home);
}

bool strictly_increasing(std::span<const double> xs) {
    for (std::size_t i = 1; i < xs.size(); ++i) {
        if (!(xs[i - 1] < xs[i])) {
            return false;
        }
    }
    return true;
}

int nearest_sample(std::span<const double> xs, double x, bool sorted,
                   std::size_t n) {
    if (n == 0 || xs.empty()) {
        return -1;
    }
    if (sorted && xs.size() == n && n >= 2u) {
        // The fast path: the axis is strictly increasing over the
        // whole span, so a binary search finds the bracketing pair and
        // the nearer of them. Ties (both equidistant) land on the
        // LOWER index - the <= in the walk - which is the scan's
        // first-strictly-closer answer for sorted inputs too.
        std::size_t lo = 0;
        std::size_t hi = n - 1;
        while (hi - lo > 1u) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            if (xs[mid] <= x) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        const double dlo = std::fabs(xs[lo] - x);
        const double dhi = std::fabs(xs[hi] - x);
        return static_cast<int>(dlo <= dhi ? lo : hi);
    }
    // The definition: nearest by absolute distance, first winner on
    // ties (a strict < compare). The wrapped .dc axis, a step family,
    // a hand-built test axis - every non-sorted shape rides this.
    std::size_t best = 0;
    double best_d = std::numeric_limits<double>::infinity();
    const std::size_t limit = std::min(n, xs.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const double d = std::fabs(xs[i] - x);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return static_cast<int>(best);
}

std::vector<std::size_t> decimate_minmax(std::span<const double> xs,
                                         std::span<const double> ys,
                                         std::size_t budget) {
    if (budget < 2u || xs.size() < 2u || xs.size() <= budget ||
        ys.size() != xs.size()) {
        return {};
    }
    // Strictly increasing xs, one pass (NaN fails the chain and lands
    // in the raw fallback with the repeated axes).
    for (std::size_t i = 1; i < xs.size(); ++i) {
        if (!(xs[i - 1] < xs[i])) {
            return {};
        }
    }

    // Buckets over the x span; each contributes its y-min and y-max
    // SAMPLE in occurrence order, so every bucket's pixels carry the
    // envelope and the global extremes survive by construction (the
    // global max IS its bucket's max). The endpoints are forced in so
    // the sweep's exact ends never drop (minmax_walk).
    const std::size_t buckets = budget / 2u;
    const double span = xs.back() - xs.front();
    if (!(span > 0.0) || !std::isfinite(span)) {
        return {};  // an overflowed span has no bucket arithmetic
    }
    const auto bucket_of = [&](std::size_t i) -> std::size_t {
        const double t = (xs[i] - xs.front()) / span;
        std::size_t b = static_cast<std::size_t>(
            t * static_cast<double>(buckets));
        if (b >= buckets) {
            b = buckets - 1u;
        }
        return b;
    };
    return minmax_walk(ys, buckets, bucket_of);
}

std::vector<std::size_t> decimate_minmax_index(
    std::span<const double> ys, std::size_t budget) {
    if (budget < 2u || ys.size() < 2u || ys.size() <= budget) {
        return {};
    }
    // The index run's bucket walk: same shape as decimate_minmax's
    // (min/max SAMPLE per bucket, occurrence order, forced endpoints)
    // but the bucket comes from the index arithmetic alone - there is
    // no x to chain, so no strictly-increasing precondition to fail.
    // A bucket that OPENS on a NaN keeps it (the finite samples behind
    // a NaN compare false against one, so the flush pushes the break
    // vertex) - the same reading decimate_minmax gives a NaN bucket.
    const std::uint64_t n = ys.size();
    const std::uint64_t buckets = budget / 2u;
    // 64-bit product: i * buckets overflows a 32-bit size_t at a few
    // million samples (the Android build).
    const auto bucket_of = [&](std::size_t i) -> std::size_t {
        std::uint64_t b = static_cast<std::uint64_t>(i) * buckets / n;
        if (b >= buckets) {
            b = buckets - 1u;
        }
        return static_cast<std::size_t>(b);
    };
    return minmax_walk(ys, static_cast<std::size_t>(buckets), bucket_of);
}

AxisRuns split_axis_runs(std::span<const double> xs) {
    AxisRuns out;
    const std::size_t n = xs.size();
    // The finite envelope: one pass over the axis for its extent.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double v : xs) {
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (lo <= hi) {
        out.finite = true;
        out.lo = lo;
        out.hi = hi;
    }
    // The run walk: a run opens at `begin` unoriented (length 1),
    // takes its direction from the first step that moves, and closes
    // where that direction fails - a reset, a plateau, a reversal, a
    // NaN on either side (both comparisons read false against one).
    // The sample that breaks a run opens the next, so the runs cover
    // [0, n) exactly; a trailing length-1 run is a legal answer.
    std::size_t begin = 0;
    bool rising = true;
    bool oriented = false;
    for (std::size_t i = 1; i <= n; ++i) {
        const bool closing = i == n;
        if (!closing) {
            if (!oriented) {
                if (xs[i] > xs[begin]) {
                    rising = true;
                    oriented = true;
                    continue;
                }
                if (xs[i] < xs[begin]) {
                    rising = false;
                    oriented = true;
                    continue;
                }
                // Equal, or a NaN on either side: a length-1 run.
            } else if (rising ? xs[i] > xs[i - 1u]
                              : xs[i] < xs[i - 1u]) {
                continue;
            }
        }
        AxisRun run;
        run.begin = begin;
        run.end = i;
        run.rising = rising;
        out.runs.push_back(run);
        begin = i;
        oriented = false;
        rising = true;
    }
    return out;
}

}  // namespace ws::plotmath
