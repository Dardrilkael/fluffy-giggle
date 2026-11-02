// apps/circsim stage 4 - the plot axis math (see include/ws/circsim/
// plot.hpp for the contracts). Pure <cmath> and <cstdio>; no ws::spice,
// no ui, so the headless suite gates every decision the Results panel
// renders.

#include "ws/circsim/plot.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ws::circsim {

namespace {

// 10^e without std::pow's round-trip surprises on exact decades: the
// table below covers every exponent the prefix walk visits, so the
// multiply stays a single rounding.
[[nodiscard]] double pow10(int e) {
    static constexpr double kTable[] = {
        1e-15, 1e-12, 1e-9, 1e-6, 1e-3, 1e0,  1e3,  1e6,
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

}  // namespace

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
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
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
    out.reserve(static_cast<std::size_t>(k1 - k0) + 1u);
    for (double k = k0; k <= k1; ++k) {
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
        char raw[48];
        std::snprintf(raw, sizeof raw, "%.*g", digits, v);
        return raw;
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
        char mant[32];
        std::snprintf(mant, sizeof mant, "%.*g", digits, scaled);
        const double printed = std::atof(mant);
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
            char raw[48];
            std::snprintf(raw, sizeof raw, "%.*g", digits, v);
            return raw;
        }
    }
    // Unreachable for inputs inside the table (the renormalized value
    // re-buckets strictly below the ladder top); kept as the honest
    // raw %g tail a promising-string function owes its callers.
    char raw[48];
    std::snprintf(raw, sizeof raw, "%.*g", digits, v);
    return raw;
}

PlotRange expand_range(double lo, double hi, double frac) {
    if (std::isnan(lo) || std::isnan(hi) || std::isinf(lo) ||
        std::isinf(hi)) {
        return {0.0, 1.0};
    }
    if (frac < 0.0 || std::isnan(frac)) {
        frac = 0.0;
    }
    if (!(lo < hi)) {
        // Degenerate: pad by frac of the magnitude floor so a constant
        // line still gets a visible axis.
        const double d = std::max(std::fabs(lo), 1.0) * frac;
        return {lo - d, lo + d};
    }
    const double pad = (hi - lo) * frac;
    return {lo - pad, hi + pad};
}

// --- the manual view (stage 5, see plot.hpp for the contracts) -----------------

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
    if (anchor < view.lo) {
        anchor = view.lo;
    }
    if (anchor > view.hi) {
        anchor = view.hi;
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
    // global max IS its bucket's max). The endpoints ride their
    // buckets but are forced in so the sweep's exact ends never drop.
    const std::size_t buckets = budget / 2u;
    const double span = xs.back() - xs.front();
    const auto bucket_of = [&](double x) -> std::size_t {
        const double t = (x - xs.front()) / span;
        std::size_t b = static_cast<std::size_t>(t *
                                                 static_cast<double>(buckets));
        if (b >= buckets) {
            b = buckets - 1u;
        }
        return b;
    };

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
        // Occurrence order: the earlier index first, so the polyline
        // through the kept points keeps the segment's true slope.
        if (min_i < max_i) {
            out.push_back(min_i);
            out.push_back(max_i);
        } else {
            out.push_back(max_i);
            out.push_back(min_i);
        }
    };
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const std::size_t b = bucket_of(xs[i]);
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

    // The kept set is bucket-ordered by construction; a boundary
    // sample can appear twice (it opened the next bucket while closing
    // the last) - harmless duplicated vertices, and cheaper than a
    // dedup pass that would need its own order proof. But the FIRST
    // and LAST samples are load-bearing for a waveform (edge timing):
    // force them exactly.
    if (out.front() != 0u) {
        out.insert(out.begin(), 0u);
    }
    if (out.back() != xs.size() - 1u) {
        out.push_back(xs.size() - 1u);
    }
    return out;
}

}  // namespace ws::circsim
