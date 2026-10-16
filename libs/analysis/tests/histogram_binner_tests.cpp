// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HistogramBinner (04 section 11.1; AC-M3-07): counts, percent and time equal a brute-force reference for
// width- and count-defined BandAxis bins with gating (resampled Previous, invalid outside the gating channel)
// and untrusted exclusion, plus the colour split; the reference walks plain arrays sample by sample with a
// linear scan of the bins, independent of the binner's chunked reads, cursors and binary search.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Plain { // a uniform channel as plain arrays
    Rate rate;
    std::vector<double> v;
    std::vector<bool> ok;
    Micros timeAt(std::size_t i) const { return rate.timeAt(static_cast<SampleIdx>(i), 0); }
};

Plain primary() {
    Plain p{Rate::fromHz(50), {}, {}};
    for (int i = 0; i < 1500; ++i) { // 30 s, m/s
        p.v.push_back(20.0 + 15.0 * std::sin(i * 0.037) + 5.0 * std::cos(i * 0.11));
        p.ok.push_back(i % 97 != 5);
    }
    return p;
}

Plain gatingChannel() {
    Plain g{Rate::fromHz(10), {}, {}};
    for (int j = 0; j < 290; ++j) { // ends at 28.9 s, before the primary
        g.v.push_back(j % 23 == 0 ? 0.0 : std::sin(j * 0.3));
        g.ok.push_back(j % 31 != 7);
    }
    return g;
}

Plain colourChannel() {
    Plain c{Rate::fromHz(20), {}, {}};
    for (int j = 0; j < 600; ++j) {
        c.v.push_back(50.0 + 49.0 * std::sin(j * 0.021));
        c.ok.push_back(true);
    }
    return c;
}

// Previous: the last sample at or before t; invalid outside [first, last].
double previousAt(const Plain& p, Micros t) {
    if (t < p.timeAt(0) || t > p.timeAt(p.v.size() - 1)) {
        return kNaN;
    }
    std::size_t j = 0;
    while (j + 1 < p.v.size() && p.timeAt(j + 1) <= t) {
        ++j;
    }
    return p.ok[j] ? p.v[j] : kNaN;
}

// Linear: the bracketing samples interpolated; invalid outside [first, last].
double linearAt(const Plain& p, Micros t) {
    if (t < p.timeAt(0) || t > p.timeAt(p.v.size() - 1)) {
        return kNaN;
    }
    std::size_t j = 0;
    while (j + 1 < p.v.size() && p.timeAt(j + 1) <= t) {
        ++j;
    }
    if (p.timeAt(j) == t) {
        return p.ok[j] ? p.v[j] : kNaN;
    }
    if (!p.ok[j] || !p.ok[j + 1]) {
        return kNaN;
    }
    const double f =
        static_cast<double>(t - p.timeAt(j)) / static_cast<double>(p.timeAt(j + 1) - p.timeAt(j));
    return p.v[j] + f * (p.v[j + 1] - p.v[j]);
}

// Bins are [lo, hi) except the last regular band, which is closed on its hi (D-194), so a sample at dataMax
// counts; the open greaterMax band then holds only the values strictly above it.
std::optional<std::size_t> scan(const std::vector<Band>& bands, double v) {
    std::optional<std::size_t> lastRegular;
    for (std::size_t k = 0; k < bands.size(); ++k) {
        if (!bands[k].lessMin && !bands[k].greaterMax) {
            lastRegular = k;
        }
    }
    if (lastRegular && v == bands[*lastRegular].hi) {
        return lastRegular;
    }
    for (std::size_t k = 0; k < bands.size(); ++k) {
        if (bands[k].lo <= v && v < bands[k].hi) {
            return k;
        }
    }
    return std::nullopt;
}

struct Brute {
    std::vector<double> counts;
    std::vector<double> seconds;
    std::vector<std::vector<double>> split;
    double total = 0.0;
};

Brute bruteForce(const Plain& p, const Plain* gating, const Plain* colour,
                 const std::vector<Band>& colourBands, double toBins, const std::vector<SampleSpan>& spans,
                 const std::vector<Band>& bins, bool excludeUntrusted) {
    Brute b;
    b.counts.assign(bins.size(), 0.0);
    b.seconds.assign(bins.size(), 0.0);
    b.split.assign(bins.size(), std::vector<double>(colourBands.size(), 0.0));
    const double period = 1.0 / p.rate.hz();
    for (const SampleSpan& span : spans) {
        if (excludeUntrusted && !span.trusted) {
            continue;
        }
        for (std::size_t i = 0; i < p.v.size(); ++i) {
            const Micros t = p.timeAt(i);
            if (t < span.start || t >= span.end || !p.ok[i]) {
                continue;
            }
            if (gating != nullptr && !(previousAt(*gating, t) > 0.0)) {
                continue;
            }
            const std::optional<std::size_t> k = scan(bins, toBins * p.v[i]);
            if (!k) {
                continue;
            }
            b.counts[*k] += 1.0;
            b.seconds[*k] += period;
            b.total += 1.0;
            if (colour != nullptr) {
                if (const std::optional<std::size_t> c = scan(colourBands, linearAt(*colour, t))) {
                    b.split[*k][*c] += 1.0;
                }
            }
        }
    }
    return b;
}

std::vector<Band> bandsOf(const std::string& axisJson) {
    auto doc = JsonDoc::parse(axisJson);
    REQUIRE(doc.ok());
    auto axis = BandAxis::fromJson(*doc);
    REQUIRE(axis.ok());
    auto bands = generateBands(*axis, 0.0, 1.0, nullptr, "");
    REQUIRE(bands.ok());
    return std::move(bands).value();
}

} // namespace

TEST_CASE("histogram: counts, percent and time equal brute force with gating and untrusted exclusion",
          "[unit][AC-M3-07]") {
    TestCache tc;
    const Plain p = primary();
    const Plain g = gatingChannel();
    const Plain c = colourChannel();
    HistogramInput input;
    input.series = tc.uniform(p.rate, p.v, p.ok);
    input.toBins = units::Converter{3.6, 0.0}; // m/s -> km/h
    input.gating = store::Series{tc.uniform(g.rate, g.v, g.ok)};
    input.colour = store::Series{tc.uniform(c.rate, c.v, c.ok)};
    input.colourBands = bandsOf(R"({"bandMode": "count", "bandCount": 4,
                                    "dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 100, "unit": ""}})");
    REQUIRE(input.colourBands.size() == 6);
    input.spans = {SampleSpan{sec(1.0), sec(9.5), true}, SampleSpan{sec(9.5), sec(18.0), false},
                   SampleSpan{sec(18.0), sec(30.0), true}};

    const std::vector<std::pair<const char*, std::vector<Band>>> binnings = {
        {"width", bandsOf(R"({"bandMode": "width", "dataMin": {"v": 30, "unit": ""},
                              "dataMax": {"v": 120, "unit": ""}, "bandWidth": {"v": 7.5, "unit": ""}})")},
        {"count", bandsOf(R"({"bandMode": "count", "bandCount": 9, "dataMin": {"v": 20, "unit": ""},
                              "dataMax": {"v": 140, "unit": ""}, "lessMinBand": false,
                              "greaterMaxBand": false})")},
    };
    for (const auto& [name, bins] : binnings) {
        for (const bool exclude : {true, false}) {
            INFO(name << (exclude ? ", untrusted excluded" : ", untrusted included"));
            const Brute want = bruteForce(p, &g, &c, input.colourBands, 3.6, input.spans, bins, exclude);
            REQUIRE(want.total > 100.0);
            for (const HistogramYMode mode :
                 {HistogramYMode::Count, HistogramYMode::Percent, HistogramYMode::Time}) {
                HistogramRequest request{bins, mode, exclude};
                auto got = HistogramBinner::bin(input, request, Priority::Visible, {});
                REQUIRE(got.ok());
                CHECK(got->counts == want.counts);
                CHECK(static_cast<double>(got->accepted) == want.total);
                CHECK(got->seconds == Catch::Approx(want.total * 0.02).epsilon(1e-12));
                REQUIRE(got->values.size() == bins.size());
                REQUIRE(got->colourSplit.size() == bins.size());
                for (std::size_t k = 0; k < bins.size(); ++k) {
                    INFO("bin " << k);
                    const double scale = mode == HistogramYMode::Count     ? 1.0
                                         : mode == HistogramYMode::Percent ? 100.0 / want.total
                                                                           : 0.02;
                    CHECK(got->values[k] == Catch::Approx(want.counts[k] * scale).epsilon(1e-12));
                    if (mode == HistogramYMode::Time) {
                        CHECK(got->values[k] == Catch::Approx(want.seconds[k]).epsilon(1e-9));
                    }
                    REQUIRE(got->colourSplit[k].size() == input.colourBands.size());
                    for (std::size_t b = 0; b < input.colourBands.size(); ++b) {
                        CHECK(got->colourSplit[k][b] ==
                              Catch::Approx(want.split[k][b] * scale).epsilon(1e-12));
                    }
                }
            }
        }
    }

    // Without gating the samples after the gating channel's last sample count again.
    HistogramInput ungated = input;
    ungated.gating.reset();
    ungated.colour.reset();
    const std::vector<Band>& bins = binnings[0].second;
    auto all = HistogramBinner::bin(ungated, HistogramRequest{bins, HistogramYMode::Count, true},
                                    Priority::Visible, {});
    REQUIRE(all.ok());
    const Brute want = bruteForce(p, nullptr, nullptr, {}, 3.6, input.spans, bins, true);
    CHECK(all->counts == want.counts);
    CHECK(all->colourSplit.empty());
}

TEST_CASE("histogram: samples at dataMin and dataMax are counted in width and count mode",
          "[unit][AC-M3-07]") {
    // A saturating channel (a brake or throttle in %): 0 and 100 are its most frequent values. Hand-counted:
    // two samples at 0, three at 100, one each at 25, 50 and 75.
    TestCache tc;
    HistogramInput input;
    input.series = tc.uniform(Rate::fromHz(10), {0, 25, 50, 100, 100, 100, 0, 75, 100});
    input.spans = {SampleSpan{0, sec(0.8), true}}; // the last 100 lies outside the span
    const auto countOf = [&](const std::vector<Band>& bins) {
        auto got = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Count, true},
                                        Priority::Visible, {});
        REQUIRE(got.ok());
        return got->counts;
    };
    SECTION("width mode, no open bands: [0,10) ... [90,100]") {
        const std::vector<Band> bins = bandsOf(
            R"({"bandMode": "width", "dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 100, "unit": ""},
                        "bandWidth": {"v": 10, "unit": ""}, "lessMinBand": false, "greaterMaxBand": false})");
        REQUIRE(bins.size() == 10);
        CHECK(countOf(bins) == std::vector<double>{2, 0, 1, 0, 0, 1, 0, 1, 0, 3});
    }
    SECTION("count mode with open bands: 100 is in the last regular band, not above it") {
        const std::vector<Band> bins = bandsOf(R"({"bandMode": "count", "bandCount": 4,
                        "dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 100, "unit": ""}})");
        REQUIRE(bins.size() == 6); // (-inf,0) [0,25) [25,50) [50,75) [75,100] (100,inf)
        CHECK(countOf(bins) == std::vector<double>{0, 2, 1, 1, 4, 0});
    }
    SECTION("automatic limits: the channel maximum lies on the last edge") {
        auto doc = JsonDoc::parse(R"({"bandMode": "count", "bandCount": 2, "lessMinBand": false,
                                      "greaterMaxBand": false})");
        REQUIRE(doc.ok());
        auto axis = BandAxis::fromJson(*doc);
        REQUIRE(axis.ok());
        auto bins = generateBands(*axis, 0.0, 100.0, nullptr, "");
        REQUIRE(bins.ok());
        CHECK(countOf(*bins) == std::vector<double>{3, 5});
    }
}

TEST_CASE("histogram: a timestamped channel holds each sample until the next one", "[unit][AC-M3-07]") {
    TestCache tc;
    HistogramInput input;
    input.series = tc.timestamped({0, 100'000, 250'000, 600'000, 1'000'000}, {1, 2, 3, 4, 5});
    input.spans = {SampleSpan{50'000, 800'000, true}};
    const std::vector<Band> bins{Band{0, 2, 0, false, false}, Band{2, 4, 0, false, false},
                                 Band{4, 6, 0, false, false}};
    auto time = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Time, true},
                                     Priority::Visible, {});
    REQUIRE(time.ok());
    // 0.1 s (2) holds to 0.25, 0.25 s (3) to 0.6, 0.6 s (4) to the span end 0.8.
    CHECK(time->counts == std::vector<double>{0, 2, 1});
    CHECK(time->values[1] == Catch::Approx(0.5));
    CHECK(time->values[2] == Catch::Approx(0.2));
    CHECK(time->seconds == Catch::Approx(0.7));
    auto percent = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Percent, true},
                                        Priority::Visible, {});
    REQUIRE(percent.ok());
    CHECK(percent->values[1] == Catch::Approx(200.0 / 3.0));

    // Timestamped gating, Previous: 0.25 s sees 0 (dropped); 0.6 s sees the 0.5 s sample.
    input.gating = store::Series{tc.timestamped({0, 200'000, 500'000, 900'000}, {1, 0, 1, 1})};
    auto gated = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Time, true},
                                      Priority::Visible, {});
    REQUIRE(gated.ok());
    CHECK(gated->counts == std::vector<double>{0, 1, 1});
    CHECK(gated->values[1] == Catch::Approx(0.15));
    // The last sample of the series credits nothing.
    input.gating.reset();
    input.spans = {SampleSpan{0, 2'000'000, true}};
    auto whole = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Time, true},
                                      Priority::Visible, {});
    REQUIRE(whole.ok());
    CHECK(whole->counts == std::vector<double>{1, 2, 2});
    CHECK(whole->values[2] == Catch::Approx(0.4)); // 4 holds 0.4 s, 5 (the last sample) 0
}

TEST_CASE("histogram: binOf, refusals and cancellation", "[unit]") {
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<Band> bins{Band{-inf, 0, 0, true, false}, Band{0, 10, 0, false, false},
                                 Band{20, 30, 0, false, false}, Band{30, inf, 0, false, true}};
    CHECK(HistogramBinner::binOf(-1e300, bins) == 0u);
    CHECK(HistogramBinner::binOf(-inf, bins) == 0u);
    CHECK(HistogramBinner::binOf(0.0, bins) == 1u);
    CHECK(HistogramBinner::binOf(9.999, bins) == 1u);
    CHECK_FALSE(HistogramBinner::binOf(10.0, bins).has_value()); // the gap [10, 20)
    CHECK(HistogramBinner::binOf(20.0, bins) == 2u);
    CHECK(HistogramBinner::binOf(30.0, bins) == 2u); // the last regular band is closed on its hi (D-194)
    CHECK(HistogramBinner::binOf(std::nextafter(30.0, inf), bins) == 3u);
    CHECK(HistogramBinner::binOf(inf, bins) == 3u);
    CHECK_FALSE(HistogramBinner::binOf(std::nan(""), bins).has_value());
    const std::span<const Band> closed(bins.data() + 1, 2);
    CHECK_FALSE(HistogramBinner::binOf(-0.5, closed).has_value());
    CHECK(HistogramBinner::binOf(30.0, closed) == 1u);
    CHECK_FALSE(HistogramBinner::binOf(std::nextafter(30.0, inf), closed).has_value());
    CHECK_FALSE(HistogramBinner::binOf(1.0, {}).has_value());

    TestCache tc;
    HistogramInput input;
    input.series = tc.uniform(Rate::fromHz(10), {1, 2, 3});
    input.spans = {SampleSpan{0, sec(1), true}};
    CHECK(HistogramBinner::bin(input, HistogramRequest{}, Priority::Visible, {}).error().code ==
          ErrorCode::Invalid);
    const std::vector<Band> backwards{Band{5, 10, 0, false, false}, Band{0, 5, 0, false, false}};
    CHECK(HistogramBinner::bin(input, HistogramRequest{backwards, HistogramYMode::Count, true},
                               Priority::Visible, {})
              .error()
              .code == ErrorCode::Invalid);
    CancelSource cancel;
    cancel.cancel();
    CHECK(HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Count, true}, Priority::Visible,
                               cancel.token())
              .error()
              .code == ErrorCode::Cancelled);
    // No accepted sample: zero percent everywhere, not NaN.
    input.spans = {SampleSpan{sec(5), sec(6), true}};
    auto none = HistogramBinner::bin(input, HistogramRequest{bins, HistogramYMode::Percent, true},
                                     Priority::Visible, {});
    REQUIRE(none.ok());
    CHECK(none->accepted == 0);
    CHECK(none->values == std::vector<double>(bins.size(), 0.0));
}
