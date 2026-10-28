// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Saturday walkthrough's data (ui.tst_walkthrough_saturday, AC-M4-01; D-214), written by the test into a
// temporary directory, never committed (header-only: included by tst_walkthrough_saturday.cpp alone, after
// the qtmodel test helpers it uses):
//   * the day's .ld + .ldx: the true centreline of the AC-M3-04 synthetic circuit (about 2975 m;
//     fixtures/track/synthetic-circuit) driven by an out lap, flying laps of 70, 68 and 68.5 s and an in lap,
//     each lap the same profile in its lap fraction u: speed v = L / P (1 + 0.3 sin 2 pi u) (so a lap
//     measures exactly L), throttle 100 max(0, cos 2 pi u) %, brake 100 max(0, -cos 2 pi u) %, gear 1 +
//     floor(v / 10) (1 to 6), engine speed 1000 + 150 v rpm, the G forces and the steering from v and the
//     centreline's curvature, GPS from the centreline at 10 Hz; beacons at the lap boundaries go to the .ldx
//     (the shipped workbook's channel names: SPEED, THROTTLE, BRAKE, GEAR, RPMS, STEERANGLE, G Force
//     Lat/Long, the wheel speeds behind Corr Speed);
//   * a second logger's export in the legacy CSV dialect (motec.csv, through Esses' own writer): Oil Pressure
//     300 + 2 t kPa and Water Temp 80 + 0.1 t deg C at 20 Hz for 30 s;
//   * a generic CSV whose Boost column has a unit nobody knows ("zorks"): the Mapping panel must resolve it.
// Every expected value of the walkthrough comes from these formulas (D-041), never from the code under test.
#pragma once

#include "../../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "../../../libs/qtmodel/tests/reports/report_track_fixtures.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/importer.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/source.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esses::saturday {

inline constexpr double kG = 9.80665;
inline constexpr unsigned kRateHz = 50;
inline constexpr unsigned kGpsHz = 10;
// Out lap, three flying laps (the fastest 68 s; 68.5 s lies within the default 1 % fast-lap tolerance, 70 s
// does not), in lap.
inline const std::vector<double> kLapSeconds{80.0, 70.0, 68.0, 68.5, 85.0};
inline constexpr std::size_t kFastest = 2; // displayed lap index of the 68 s lap
inline const std::string kVenue = "Saturday Circuit";
inline const std::string kDriver = "Saturday Driver";

struct Circuit {
    double lengthM = 0.0;
    std::vector<std::pair<double, double>> centre; // ENU m, every 1 m, closed
    double lat0 = 0.0;
    double lon0 = 0.0;

    std::pair<double, double> at(double s) const {
        const double n = static_cast<double>(centre.size() - 1);
        double x = std::fmod(s, lengthM);
        if (x < 0.0) {
            x += lengthM;
        }
        const double pos = x / lengthM * n;
        const auto i = std::min(static_cast<std::size_t>(pos), centre.size() - 2);
        const double f = pos - static_cast<double>(i);
        return {centre[i].first + f * (centre[i + 1].first - centre[i].first),
                centre[i].second + f * (centre[i + 1].second - centre[i].second)};
    }
    double heading(double s) const {
        const auto a = at(s - 0.5);
        const auto b = at(s + 0.5);
        return std::atan2(b.second - a.second, b.first - a.first);
    }
    // Curvature (1/m, positive turning left) over a 10 m baseline.
    double curvature(double s) const {
        double d = heading(s + 5.0) - heading(s - 5.0);
        while (d > std::numbers::pi) {
            d -= 2.0 * std::numbers::pi;
        }
        while (d < -std::numbers::pi) {
            d += 2.0 * std::numbers::pi;
        }
        return d / 10.0;
    }
};

inline Circuit readCircuit() {
    const qt::rttest::CircuitTruth truth =
        qt::rttest::readCircuitTruth(qt::test::repoRoot() / "fixtures" / "track" / "synthetic-circuit");
    Circuit c;
    c.lengthM = truth.lengthM;
    c.centre = truth.centre;
    c.lat0 = truth.originLat;
    c.lon0 = truth.originLon;
    return c;
}

inline double totalSeconds() {
    double total = 0.0;
    for (const double s : kLapSeconds) {
        total += s;
    }
    return total;
}

// The lap (0 = out lap) and its fraction at time t (seconds from the log start).
struct LapPhase {
    std::size_t lap = 0;
    double u = 0.0;
    double period = 0.0;
    double start = 0.0;
};

inline LapPhase phaseAt(double t) {
    double start = 0.0;
    for (std::size_t k = 0; k < kLapSeconds.size(); ++k) {
        if (t < start + kLapSeconds[k] || k + 1 == kLapSeconds.size()) {
            return {k, std::min(1.0, (t - start) / kLapSeconds[k]), kLapSeconds[k], start};
        }
        start += kLapSeconds[k];
    }
    return {};
}

inline double lapStart(std::size_t lap) {
    double start = 0.0;
    for (std::size_t k = 0; k < lap; ++k) {
        start += kLapSeconds[k];
    }
    return start;
}

// The formulas (lap fraction u of a lap of period P on a circuit of length L).
inline double speedMs(double lengthM, double period, double u) {
    return lengthM / period * (1.0 + 0.3 * std::sin(2.0 * std::numbers::pi * u));
}
inline double throttlePct(double u) {
    return 100.0 * std::max(0.0, std::cos(2.0 * std::numbers::pi * u));
}
inline double brakePct(double u) {
    return 100.0 * std::max(0.0, -std::cos(2.0 * std::numbers::pi * u));
}
inline double gearOf(double v) {
    return std::clamp(1.0 + std::floor(v / 10.0), 1.0, 6.0);
}
inline double rpmOf(double v) {
    return 1000.0 + 150.0 * v;
}
// Distance into the lap at fraction u (the integral of the speed).
inline double distanceIn(double lengthM, double u) {
    return lengthM * (u + 0.3 / (2.0 * std::numbers::pi) * (1.0 - std::cos(2.0 * std::numbers::pi * u)));
}

inline model::Channel channel(const model::SourceId& src, const char* name, const char* unit, Rate rate,
                              std::vector<float> values, store::ChunkCache& cache, int dps) {
    const units::UnitRegistry& units = *qt::test::shippedUnits();
    const units::UnitId id = unit[0] == '\0' ? units::kNoUnit : *units.find(unit);
    return qt::rttest::uniformChannel(src, name, id, units, rate, std::move(values), cache, dps);
}

// The day's log as a source (the .ld writer's input).
inline std::shared_ptr<const model::DataSource> daySource(const Circuit& c, store::ChunkCache& cache,
                                                          const std::string& file = "saturday.ld") {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view(file));
    src->prov.path = pathFromUtf8(file);
    src->prov.importerId = "motec.ld";
    src->prov.hash = src->id;
    const double total = totalSeconds();
    const auto n = static_cast<std::size_t>(std::llround(total * kRateHz));
    std::vector<float> fl(n), fr(n), speed(n), throttle(n), brake(n), gear(n), rpm(n), latG(n), longG(n),
        steer(n);
    const double L = c.lengthM;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRateHz;
        const LapPhase p = phaseAt(t);
        const double w = 2.0 * std::numbers::pi * p.u;
        const double v = speedMs(L, p.period, p.u);
        const double a = L / p.period * 0.3 * (2.0 * std::numbers::pi / p.period) * std::cos(w);
        const double kappa = c.curvature(distanceIn(L, p.u));
        fl[i] = static_cast<float>(v);
        fr[i] = static_cast<float>(0.97 * v);
        speed[i] = static_cast<float>(v);
        throttle[i] = static_cast<float>(throttlePct(p.u));
        brake[i] = static_cast<float>(brakePct(p.u));
        gear[i] = static_cast<float>(gearOf(v));
        rpm[i] = static_cast<float>(rpmOf(v));
        latG[i] = static_cast<float>(v * v * kappa / kG);
        longG[i] = static_cast<float>(a / kG);
        steer[i] = static_cast<float>(kappa * 2.7 * 14.0 * 180.0 / std::numbers::pi);
    }
    const auto ng = static_cast<std::size_t>(std::llround(total * kGpsHz));
    std::vector<double> lat(ng);
    std::vector<double> lon(ng);
    constexpr double kMetresPerDegree = 111'320.0;
    for (std::size_t i = 0; i < ng; ++i) {
        const LapPhase p = phaseAt(static_cast<double>(i) / kGpsHz);
        const auto [x, y] = c.at(distanceIn(L, p.u));
        lat[i] = c.lat0 + y / kMetresPerDegree;
        lon[i] = c.lon0 + x / (kMetresPerDegree * std::cos(c.lat0 * std::numbers::pi / 180.0));
    }
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = static_cast<Micros>(std::llround(total * 1e6));
    const Rate rate{kRateHz, 1};
    layer.addChannel(channel(src->id, "Wheel Speed FL", "m/s", rate, fl, cache, 2));
    layer.addChannel(channel(src->id, "Wheel Speed FR", "m/s", rate, fr, cache, 2));
    layer.addChannel(channel(src->id, "SPEED", "m/s", rate, speed, cache, 2));
    layer.addChannel(channel(src->id, "THROTTLE", "%", rate, throttle, cache, 1));
    layer.addChannel(channel(src->id, "BRAKE", "%", rate, brake, cache, 1));
    layer.addChannel(channel(src->id, "GEAR", "", rate, gear, cache, 0));
    layer.addChannel(channel(src->id, "RPMS", "rpm", rate, rpm, cache, 0));
    layer.addChannel(channel(src->id, "STEERANGLE", "deg", rate, steer, cache, 1));
    layer.addChannel(channel(src->id, "G Force Lat", "G", rate, latG, cache, 2));
    layer.addChannel(channel(src->id, "G Force Long", "G", rate, longG, cache, 2));
    const auto gps = [&](const char* name, const std::vector<double>& values) {
        model::Channel ch = channel(src->id, name, "deg", Rate{kGpsHz, 1}, {}, cache, 7);
        store::ColumnOptions co;
        co.cache = &cache;
        ch.desc.dtype = store::Dtype::F64;
        ch.series = store::UniformSeries{
            Rate{kGpsHz, 1}, 0,
            store::Column(store::InMemoryProvider::fromF64(values, {}, std::string("memory:") + name), co)};
        layer.addChannel(std::move(ch));
    };
    gps("GPS Latitude", lat);
    gps("GPS Longitude", lon);
    layer.markers.push_back(
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived));
    double boundary = 0.0;
    for (std::size_t k = 0; k + 1 < kLapSeconds.size(); ++k) {
        boundary += kLapSeconds[k];
        layer.markers.push_back(model::makeLoggedMarker(
            model::MarkerClass::BCN, static_cast<Micros>(std::llround(boundary * 1e6)), "Beacons", "Beacon",
            {}, 77, 0, model::Provenance::Companion));
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, layer.duration, "", "", "", 0, 0,
                                                    model::Provenance::Derived));
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    src->layers.push_back(std::move(layer));
    src->details.push_back(qt::rttest::textDetail("Venue", kVenue));
    src->details.push_back(qt::rttest::textDetail("Driver", kDriver));
    src->baseDetails = src->details;
    analysis::RuleRunner::circuitDefault().run(*src);
    return src;
}

// The second logger's export: Oil Pressure 300 + 2 t kPa, Water Temp 80 + 0.1 t deg C, 20 Hz, 30 s.
inline constexpr unsigned kCsvRateHz = 20;
inline constexpr double kCsvSeconds = 30.0;
inline double oilKpa(double t) {
    return 300.0 + 2.0 * t;
}
inline double waterC(double t) {
    return 80.0 + 0.1 * t;
}

inline std::filesystem::path writeLegacyCsv(const std::filesystem::path& dir, store::ChunkCache& cache) {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view("second-logger.csv"));
    src->prov.path = "second-logger.csv";
    src->prov.importerId = "motec.csv";
    src->prov.hash = src->id;
    const auto n = static_cast<std::size_t>(std::llround(kCsvSeconds * kCsvRateHz));
    std::vector<float> oil(n);
    std::vector<float> water(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kCsvRateHz;
        oil[i] = static_cast<float>(oilKpa(t));
        water[i] = static_cast<float>(waterC(t));
    }
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = static_cast<Micros>(std::llround(kCsvSeconds * 1e6));
    const Rate rate{kCsvRateHz, 1};
    // The values are in the channels' units here; the writer converts from the base unit, so store base.
    const units::UnitRegistry& units = *qt::test::shippedUnits();
    std::vector<float> oilPa(n);
    std::vector<float> waterK(n);
    const units::Converter kpa = units.toBase(*units.find("kPa"));
    const units::Converter degC = units.toBase(*units.find("deg C"));
    for (std::size_t i = 0; i < n; ++i) {
        oilPa[i] = static_cast<float>(kpa(oil[i]));
        waterK[i] = static_cast<float>(degC(water[i]));
    }
    layer.addChannel(channel(src->id, "Oil Pressure", "Pa", rate, oilPa, cache, 1));
    layer.addChannel(channel(src->id, "Water Temp", "K", rate, waterK, cache, 1));
    src->layers.push_back(std::move(layer));
    src->details.push_back(qt::rttest::textDetail("Venue", kVenue));
    src->details.push_back(qt::rttest::textDetail("Driver", kDriver));
    src->baseDetails = src->details;

    const std::filesystem::path out = dir / "second-logger.csv";
    formats::ExportView view;
    view.source = src;
    view.resampleRate = rate;
    formats::WriterOptions options;
    Result<model::OpaqueJson> unitsJson =
        model::OpaqueJson::parse(R"({"csv": {"units": {"Oil Pressure": "kPa", "Water Temp": "deg C"}}})");
    if (unitsJson) {
        options.options = std::move(unitsJson).value();
    }
    auto written = formats::makeLegacyCsvWriter(&units)->write(out, view, options, {}, {});
    if (!written) {
        throw std::runtime_error("cannot write " + pathToUtf8(out) + ": " + written.error().toString());
    }
    return out;
}

// A generic CSV with one unknown unit (Boost in "zorks") and one known (Coolant in deg C), 2 Hz.
inline std::filesystem::path writeUnknownUnitsCsv(const std::filesystem::path& dir) {
    const std::filesystem::path p = dir / "boost-logger.csv";
    std::ofstream out(p, std::ios::binary);
    out << "Time,Boost,Coolant\ns,zorks,deg C\n";
    for (int i = 0; i <= 20; ++i) {
        out << i * 0.5 << "," << 100 + 5 * i << "," << 70 + i << "\n";
    }
    return p;
}

} // namespace esses::saturday
