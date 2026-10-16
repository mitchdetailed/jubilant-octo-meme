// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Header-only fixtures of the reports and track tests (qtmodel tests/reports and tests/track, and the
// widgets' components/reports_track tests, which include this file): a source whose range tree is written by
// hand (one outing, laps, Default track sections) with two channels whose values are known in closed form, so
// every expected cell is computed by hand in the tests (D-041: never from the code under test), and a
// synthetic oval track map with hand-placed sections.
//   "Speed" (m/s) sample i = i (i = t * rate), "Load" (%) sample i = 2 i, F32, uniform at `rateHz`.
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/time.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::qt::rttest {

inline constexpr std::string_view kOutings = "Outings";
inline constexpr std::string_view kLaps = "Outings:Laps";
inline constexpr std::string_view kSectionsParent = "Outings:Laps:Track Sections";
inline constexpr std::string_view kSections = "Outings:Laps:Track Sections:Default";

inline Micros secs(double s) {
    return static_cast<Micros>(std::llround(s * 1e6));
}

struct RangeSpec {
    std::string name;
    double startS = 0.0;
    double endS = 0.0;
    bool trusted = true;
};

inline model::Range makeRange(std::string_view group, const RangeSpec& r, model::RangeKind kind) {
    model::Range out;
    out.kind = kind;
    out.name = r.name;
    out.start = secs(r.startS);
    out.end = secs(r.endS);
    out.trusted = r.trusted;
    out.id = model::rangeId(group, model::MarkerId{}, r.name + "@" + std::to_string(out.start));
    return out;
}

inline void addGroup(model::DataLayer& layer, std::string_view path, const std::vector<RangeSpec>& ranges,
                     model::RangeKind kind) {
    model::RangeGroup g = model::RangeGroup::make(std::string(path));
    for (const RangeSpec& r : ranges) {
        g.ranges.push_back(makeRange(path, r, kind));
    }
    g.version = 1;
    if (!layer.ranges.setGroup(std::move(g))) {
        throw std::runtime_error("cannot add group " + std::string(path));
    }
}

inline model::Channel uniformChannel(const model::SourceId& src, const std::string& name, units::UnitId unit,
                                     const units::UnitRegistry& units, Rate rate, std::vector<float> values,
                                     store::ChunkCache& cache, int dps = 2) {
    model::Channel c;
    c.desc.name = name;
    c.desc.loggedName = name;
    c.desc.id = model::channelIdLogged(src, model::kNormalLayer, name, "motec.ld");
    c.desc.unit = unit;
    c.desc.quantity = units.quantityOf(unit);
    c.desc.dtype = store::Dtype::F32;
    c.desc.rate = rate;
    c.desc.dps = dps;
    store::ColumnOptions co;
    co.cache = &cache;
    c.series = store::UniformSeries{
        rate, 0, store::Column(store::InMemoryProvider::fromF32(values, {}, "memory:" + name), co)};
    return c;
}

inline model::Detail textDetail(std::string id, std::string value) {
    return model::Detail{std::move(id), std::move(value), model::Provenance::Companion, std::nullopt};
}

// A source of `durationS` seconds: "Speed" and "Load" as above, the outing [0, durationS) named `outing`,
// the laps and Default sections given, BCN markers at every lap start but the first, the details given.
inline std::shared_ptr<const model::DataSource>
reportSource(const std::string& tag, double durationS, const std::vector<RangeSpec>& laps,
             const std::vector<RangeSpec>& sections, std::vector<model::Detail> details,
             const units::UnitRegistry& units, store::ChunkCache& cache, unsigned rateHz = 10,
             const std::string& outing = "test.ld") {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view(tag));
    src->prov.path = tag + ".ld";
    src->prov.importerId = "motec.ld";
    src->prov.hash = src->id;
    const Rate rate{rateHz, 1};
    const auto n = static_cast<std::size_t>(std::llround(durationS * rateHz));
    std::vector<float> speed(n);
    std::vector<float> load(n);
    for (std::size_t i = 0; i < n; ++i) {
        speed[i] = static_cast<float>(i);
        load[i] = static_cast<float>(2 * i);
    }
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = secs(durationS);
    layer.addChannel(uniformChannel(src->id, "Speed", *units.find("m/s"), units, rate, speed, cache));
    layer.addChannel(uniformChannel(src->id, "Load", *units.find("%"), units, rate, load, cache, 1));
    for (std::size_t k = 1; k < laps.size(); ++k) {
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, secs(laps[k].startS),
                                                        "Beacons", "Beacon", {}, 77, 0,
                                                        model::Provenance::Companion));
    }
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    addGroup(layer, kOutings, {RangeSpec{outing, 0.0, durationS, true}}, model::RangeKind::Outing);
    addGroup(layer, kLaps, laps, model::RangeKind::Lap);
    addGroup(layer, kSectionsParent, {}, model::RangeKind::Section);
    addGroup(layer, kSections, sections, model::RangeKind::Section);
    src->layers.push_back(std::move(layer));
    src->details = std::move(details);
    src->baseDetails = src->details;
    return src;
}

inline model::RangeRef refOf(const model::DataSource& src, std::string_view group, std::string_view name) {
    const model::RangeGroup* g = src.layers.front().ranges.find(group);
    if (g != nullptr) {
        for (const model::Range& r : g->ranges) {
            if (r.name == name) {
                model::RangeRef ref;
                ref.src = src.id;
                ref.layer = std::string(model::kNormalLayer);
                ref.groupPath = std::string(group);
                ref.range = r.id;
                ref.name = r.name;
                return ref;
            }
        }
    }
    throw std::runtime_error("no range " + std::string(name) + " in " + std::string(group));
}

// A closed oval (two straights of `straightM`, two half circles of radius `radiusM`), counter-clockwise,
// one point per metre, starting at the middle of the bottom straight heading east; Default sections: the
// two straights and the two turns, each named and tiled by hand.
inline analysis::TrackMap ovalMap(const std::string& venue, double straightM = 400.0, double radiusM = 80.0) {
    analysis::TrackMap map;
    map.name = venue.empty() ? std::string("Oval") : venue;
    map.venue = venue;
    map.direction = analysis::TrackDirection::CounterClockwise;
    map.closed = true;
    const double turn = std::numbers::pi * radiusM;
    const double length = 2.0 * straightM + 2.0 * turn;
    map.lengthM = length;
    const auto pointAt = [&](double d) {
        // bottom straight from x = 0 to straightM/2, turn 1, top straight, turn 2, back to x = 0
        const double half = straightM / 2.0;
        if (d < half) {
            return analysis::TrackPoint{d, 0.0, d};
        }
        d -= half;
        if (d < turn) {
            const double a = -std::numbers::pi / 2.0 + d / radiusM;
            return analysis::TrackPoint{half + radiusM * std::cos(a), radiusM + radiusM * std::sin(a), 0.0};
        }
        d -= turn;
        if (d < straightM) {
            return analysis::TrackPoint{half - d, 2.0 * radiusM, 0.0};
        }
        d -= straightM;
        if (d < turn) {
            const double a = std::numbers::pi / 2.0 + d / radiusM;
            return analysis::TrackPoint{-half + radiusM * std::cos(a), radiusM + radiusM * std::sin(a), 0.0};
        }
        d -= turn;
        return analysis::TrackPoint{-half + d, 0.0, 0.0};
    };
    const auto steps = static_cast<int>(std::floor(length));
    for (int i = 0; i <= steps; ++i) {
        analysis::TrackPoint p = pointAt(static_cast<double>(i));
        p.dist = static_cast<double>(i);
        map.centre.push_back(p);
    }
    analysis::TrackPoint last = map.centre.front();
    last.dist = length;
    map.centre.push_back(last); // a closed map ends at its start point again
    const double half = straightM / 2.0;
    const auto section = [&](std::string id, std::string name, double a, double b, std::string category) {
        analysis::TrackSection s;
        s.id = std::move(id);
        s.name = std::move(name);
        s.startM = a;
        s.endM = b;
        s.category = std::move(category);
        map.sections.push_back(std::move(s));
    };
    section("s1", "Straight 1", 0.0, half, "straight");
    section("t1", "Turn 1", half, half + turn, "corner");
    section("s2", "Straight 2", half + turn, half + turn + straightM, "straight");
    section("t2", "Turn 2", half + turn + straightM, half + 2.0 * turn + straightM, "corner");
    section("s3", "Straight 3", half + 2.0 * turn + straightM, length, "straight");
    return map;
}

// A closed rectangle `w` x `h` metres, counter-clockwise, starting at the middle of the bottom side heading
// east, one integer point per metre (no trigonometry: the DrawList goldens stay bit-exact across compilers
// and C libraries); Default sections by hand: a "Turn n" of 2 * `turnM` around each corner and the straights
// between ("Straight 1" from the start line, "Straight 5" back to it).
inline analysis::TrackMap rectMap(const std::string& venue, int w = 700, int h = 300, int turnM = 50) {
    analysis::TrackMap map;
    map.name = venue.empty() ? std::string("Rectangle") : venue;
    map.venue = venue;
    map.direction = analysis::TrackDirection::CounterClockwise;
    map.closed = true;
    const int half = w / 2;
    const int length = 2 * (w + h);
    map.lengthM = length;
    for (int d = 0; d <= length; ++d) {
        int x = 0;
        int y = 0;
        if (d <= half) {
            x = half + d;
        } else if (d <= half + h) {
            x = w;
            y = d - half;
        } else if (d <= half + h + w) {
            x = w - (d - half - h);
            y = h;
        } else if (d <= half + 2 * h + w) {
            y = h - (d - half - h - w);
        } else {
            x = d - half - 2 * h - w;
        }
        map.centre.push_back(
            analysis::TrackPoint{static_cast<double>(x), static_cast<double>(y), static_cast<double>(d)});
    }
    const int corners[] = {half, half + h, half + h + w, half + 2 * h + w};
    int from = 0;
    for (int i = 0; i < 4; ++i) {
        analysis::TrackSection straight;
        straight.id = "s" + std::to_string(i + 1);
        straight.name = "Straight " + std::to_string(i + 1);
        straight.startM = from;
        straight.endM = corners[i] - turnM;
        straight.category = "straight";
        map.sections.push_back(straight);
        analysis::TrackSection turn;
        turn.id = "t" + std::to_string(i + 1);
        turn.name = "Turn " + std::to_string(i + 1);
        turn.startM = corners[i] - turnM;
        turn.endM = corners[i] + turnM;
        turn.category = "corner";
        map.sections.push_back(turn);
        from = corners[i] + turnM;
    }
    analysis::TrackSection last;
    last.id = "s5";
    last.name = "Straight 5";
    last.startM = from;
    last.endM = length;
    last.category = "straight";
    map.sections.push_back(last);
    return map;
}

// A source whose laps are driven at a constant speed each (no trigonometry anywhere): "Wheel Speed FL" and
// "Wheel Speed FR" (m/s) = lengthM / lap time, so Corr Dist grows linearly through every lap; BCN markers
// between the laps, the laps from the circuit rules, a Venue detail.
inline std::shared_ptr<model::DataSource> linearSource(const std::string& tag, const std::string& venue,
                                                       const std::vector<double>& lapSeconds, double lengthM,
                                                       const units::UnitRegistry& units,
                                                       store::ChunkCache& cache, unsigned rateHz = 20) {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view(tag));
    src->prov.path = tag + ".ld";
    src->prov.importerId = "motec.ld";
    src->prov.hash = src->id;
    double total = 0.0;
    for (const double s : lapSeconds) {
        total += s;
    }
    const auto n = static_cast<std::size_t>(std::llround(total * rateHz));
    std::vector<float> v(n);
    double lapStart = 0.0;
    std::size_t lap = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rateHz;
        while (lap + 1 < lapSeconds.size() && t >= lapStart + lapSeconds[lap]) {
            lapStart += lapSeconds[lap];
            ++lap;
        }
        v[i] = static_cast<float>(lengthM / lapSeconds[lap]);
    }
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = secs(total);
    const Rate rate{rateHz, 1};
    layer.addChannel(uniformChannel(src->id, "Wheel Speed FL", *units.find("m/s"), units, rate, v, cache));
    layer.addChannel(uniformChannel(src->id, "Wheel Speed FR", *units.find("m/s"), units, rate, v, cache));
    layer.markers.push_back(
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived));
    double boundary = 0.0;
    for (std::size_t k = 0; k + 1 < lapSeconds.size(); ++k) {
        boundary += lapSeconds[k];
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, secs(boundary), "Beacons",
                                                        "Beacon", {}, 77, 0, model::Provenance::Companion));
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, secs(total), "", "", "", 0, 0,
                                                    model::Provenance::Derived));
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    src->layers.push_back(std::move(layer));
    src->details.push_back(textDetail("Venue", venue));
    src->baseDetails = src->details;
    analysis::RuleRunner::circuitDefault().run(*src);
    return src;
}

// ---- the synthetic circuit of AC-M3-04 (fixtures/track/synthetic-circuit, tools/gen_track_fixtures.py)
// -----

struct CircuitTruth {
    double originLat = 0.0; // degrees
    double originLon = 0.0;
    double lengthM = 0.0;
    Micros lapStart = 0;
    Micros lapEnd = 0;
    Micros signalEnd = 0;
    std::vector<std::pair<double, double>> centre; // ENU metres about the fixture's origin, closed
};

// A fixture file's bytes (esses::readWholeFile: never a raw stream, the PR 2 CI rule).
inline std::string readFixtureText(const std::filesystem::path& path) {
    Result<std::string> text = readWholeFile(path);
    if (!text) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::move(text).value();
}

// The rows of a fixture CSV (header skipped); an empty cell is NaN.
inline std::vector<std::vector<double>> readCsv(const std::filesystem::path& path) {
    std::istringstream in(readFixtureText(path));
    std::vector<std::vector<double>> rows;
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::vector<double> row;
        std::size_t from = 0;
        while (true) {
            const std::size_t comma = line.find(',', from);
            const std::string cell =
                line.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
            row.push_back(cell.empty() ? std::numeric_limits<double>::quiet_NaN()
                                       : std::strtod(cell.c_str(), nullptr));
            if (comma == std::string::npos) {
                break;
            }
            from = comma + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// The origin, lengthM and the lap window from circuit.json, the true centreline from expected.centre.csv.
inline CircuitTruth readCircuitTruth(const std::filesystem::path& dir) {
    QJsonParseError error{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(readFixtureText(dir / "circuit.json")), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        throw std::runtime_error("circuit.json is not a JSON object");
    }
    const QJsonObject root = doc.object();
    const auto number = [](const QJsonObject& o, const char* key) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isDouble()) {
            throw std::runtime_error(std::string("circuit.json has no number ") + key);
        }
        return v.toDouble();
    };
    const QJsonObject origin = root.value(QStringLiteral("origin")).toObject();
    const QJsonObject lap = root.value(QStringLiteral("lap")).toObject();
    CircuitTruth t;
    t.originLat = number(origin, "lat");
    t.originLon = number(origin, "lon");
    t.lengthM = number(root, "lengthM");
    t.lapStart = static_cast<Micros>(number(lap, "startUs"));
    t.lapEnd = static_cast<Micros>(number(lap, "endUs"));
    t.signalEnd = static_cast<Micros>(number(lap, "signalEndUs"));
    for (const std::vector<double>& r : readCsv(dir / "expected.centre.csv")) {
        t.centre.emplace_back(r.at(1), r.at(2));
    }
    return t;
}

// The fixture lap as a source: "GPS Latitude"/"GPS Longitude" (deg, F64, 10 Hz, the dropout invalid),
// "IMU Speed" (m/s) and "G Force Lat" (m/s/s, positive to the left) at 50 Hz; laps "Out Lap" [0, start),
// "Lap 1" [start, end), "In Lap" [end, signal end) with BCN markers at start and end; a Venue detail.
inline std::shared_ptr<const model::DataSource> circuitSource(const std::filesystem::path& dir,
                                                              const std::string& venue,
                                                              const units::UnitRegistry& units,
                                                              store::ChunkCache& cache) {
    const CircuitTruth truth = readCircuitTruth(dir);
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view("synthetic-circuit/" + venue));
    src->prov.path = "synthetic-circuit.ld";
    src->prov.importerId = "motec.ld";
    src->prov.hash = src->id;
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = truth.signalEnd;
    std::vector<double> lat;
    std::vector<double> lon;
    std::vector<bool> fix;
    for (const std::vector<double>& r : readCsv(dir / "lap.gps.csv")) {
        const bool ok = std::isfinite(r.at(1)) && std::isfinite(r.at(2));
        lat.push_back(ok ? r[1] : 0.0);
        lon.push_back(ok ? r[2] : 0.0);
        fix.push_back(ok);
    }
    std::vector<float> speed;
    std::vector<float> latG;
    for (const std::vector<double>& r : readCsv(dir / "lap.imu.csv")) {
        speed.push_back(static_cast<float>(r.at(1)));
        latG.push_back(static_cast<float>(r.at(2)));
    }
    const auto f64 = [&](const std::string& name, std::vector<double> values, std::vector<bool> valid) {
        model::Channel c =
            uniformChannel(src->id, name, *units.find("deg"), units, Rate{10, 1}, {}, cache, 7);
        store::ColumnOptions co;
        co.cache = &cache;
        c.desc.dtype = store::Dtype::F64;
        c.series = store::UniformSeries{
            Rate{10, 1}, 0,
            store::Column(store::InMemoryProvider::fromF64(values, std::move(valid), "memory:" + name), co)};
        return c;
    };
    layer.addChannel(f64("GPS Latitude", lat, fix));
    layer.addChannel(f64("GPS Longitude", lon, fix));
    layer.addChannel(
        uniformChannel(src->id, "IMU Speed", *units.find("m/s"), units, Rate{50, 1}, speed, cache));
    layer.addChannel(
        uniformChannel(src->id, "G Force Lat", *units.find("m/s/s"), units, Rate{50, 1}, latG, cache));
    for (const Micros t : {truth.lapStart, truth.lapEnd}) {
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, t, "Beacons", "Beacon", {},
                                                        77, 0, model::Provenance::Companion));
    }
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    const double startS = static_cast<double>(truth.lapStart) / 1e6;
    const double endS = static_cast<double>(truth.lapEnd) / 1e6;
    const double signalS = static_cast<double>(truth.signalEnd) / 1e6;
    addGroup(layer, kOutings, {RangeSpec{"synthetic-circuit.ld", 0.0, signalS, true}},
             model::RangeKind::Outing);
    addGroup(layer, kLaps,
             {RangeSpec{"Out Lap", 0.0, startS, false}, RangeSpec{"Lap 1", startS, endS, true},
              RangeSpec{"In Lap", endS, signalS, false}},
             model::RangeKind::Lap);
    src->layers.push_back(std::move(layer));
    src->details.push_back(textDetail("Venue", venue));
    src->baseDetails = src->details;
    return src;
}

// RMS of the distances from each point to the closest segment of a closed polyline (brute force).
inline double rmsToPolyline(const std::vector<analysis::TrackPoint>& pts,
                            const std::vector<std::pair<double, double>>& line) {
    double sum = 0.0;
    for (const analysis::TrackPoint& p : pts) {
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            const double ax = line[i].first;
            const double ay = line[i].second;
            const double dx = line[i + 1].first - ax;
            const double dy = line[i + 1].second - ay;
            const double len2 = dx * dx + dy * dy;
            const double u =
                len2 > 0.0 ? std::clamp(((p.x - ax) * dx + (p.y - ay) * dy) / len2, 0.0, 1.0) : 0.0;
            const double ex = ax + u * dx - p.x;
            const double ey = ay + u * dy - p.y;
            best = std::min(best, ex * ex + ey * ey);
        }
        sum += best;
    }
    return pts.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(pts.size()));
}

} // namespace esses::qt::rttest
