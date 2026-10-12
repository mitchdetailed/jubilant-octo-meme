// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers of the esses_analysis tests: markers and layers built in memory, uniform and timestamped
// series over a test-local chunk cache, the repository root and a property-test seed.
#pragma once

#include <esses/analysis/analysis.hpp>
#include <esses/base/env.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace esses::analysis::test {

inline std::filesystem::path repoRoot() {
#ifdef ESSES_TEST_REPO_ROOT
    return pathFromUtf8(ESSES_TEST_REPO_ROOT);
#else
    return {};
#endif
}

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1011ull;
}

constexpr Micros sec(double s) {
    return static_cast<Micros>(s * 1'000'000.0 + (s >= 0 ? 0.5 : -0.5));
}

inline model::Marker intrinsic(model::MarkerClass cls, Micros t, std::uint32_t ordinal = 0) {
    return model::makeLoggedMarker(cls, t, "", "", "", 0, ordinal, model::Provenance::Derived);
}

inline model::Marker beacon(Micros t, std::string name, model::MarkerClass cls = model::MarkerClass::BCN) {
    return model::makeLoggedMarker(cls, t, "Beacons", std::move(name), "", 77, 0,
                                   model::Provenance::Companion);
}

inline model::Marker reset(Micros t, std::string name = "Reset") {
    return model::makeLoggedMarker(model::MarkerClass::RESET, t, "", std::move(name), "", 0, 0,
                                   model::Provenance::Companion);
}

// A layer "Normal" of the given duration with the markers sorted into model order.
inline model::DataLayer layerWith(Micros duration, std::vector<model::Marker> markers) {
    model::DataLayer layer;
    layer.duration = duration;
    layer.markers = std::move(markers);
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    return layer;
}

// A source with one layer and a File Name detail.
inline model::DataSource sourceWith(model::DataLayer layer, std::vector<model::Detail> details = {}) {
    model::DataSource src;
    src.layers.push_back(std::move(layer));
    details.push_back(
        model::Detail{"File Name", std::string("test.ld"), model::Provenance::Derived, std::nullopt});
    src.details = std::move(details);
    src.baseDetails = src.details;
    return src;
}

inline model::Detail textDetail(std::string id, std::string value) {
    return model::Detail{std::move(id), std::move(value), model::Provenance::Companion, std::nullopt};
}

// A chunk cache for the columns a test creates (columns keep it alive only by pointer).
struct TestCache {
    store::ChunkCache cache;
    store::ColumnOptions options() {
        store::ColumnOptions o;
        o.cache = &cache;
        return o;
    }
    store::UniformSeries uniform(Rate rate, const std::vector<double>& values, std::vector<bool> valid = {},
                                 Micros t0 = 0) {
        return store::UniformSeries{
            rate, t0,
            store::Column(store::InMemoryProvider::fromF64(values, std::move(valid), "test:uniform"),
                          options())};
    }
    store::UniformSeries uniformF32(Rate rate, const std::vector<float>& values,
                                    std::vector<bool> valid = {}) {
        return store::UniformSeries{
            rate, 0,
            store::Column(store::InMemoryProvider::fromF32(values, std::move(valid), "test:uniform32"),
                          options())};
    }
    store::TimestampedSeries timestamped(const std::vector<std::int64_t>& times,
                                         const std::vector<double>& values) {
        store::Column t(store::InMemoryProvider::fromI64(times, "test:times"), options());
        store::Column v(store::InMemoryProvider::fromF64(values, {}, "test:values"), options());
        auto made = store::makeTimestampedSeries(std::move(t), std::move(v));
        return std::move(made).value();
    }
};

// A channel of the layer over `series` (Real, base units, no unit).
inline model::Channel channel(std::string name, store::Series series) {
    model::Channel c;
    c.desc.name = name;
    c.desc.loggedName = std::move(name);
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        c.desc.rate = u->rate;
    }
    c.series = std::move(series);
    return c;
}

// The laps group of a layer, or nullptr.
inline const model::RangeGroup* lapsOf(const model::DataLayer& layer) {
    return layer.ranges.find("Outings:Laps");
}

} // namespace esses::analysis::test
