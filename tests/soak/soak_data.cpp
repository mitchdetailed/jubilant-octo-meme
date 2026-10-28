// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "soak_data.hpp"

#include <esses/base/hash.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/formats/registry.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

namespace esses::soak {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kG = 9.80665;
constexpr double kMetresPerDegree = 111'320.0;
constexpr double kA = 0.30; // the speed profile's main wave
constexpr double kB = 0.08; // and its third harmonic

Micros micros(double s) {
    return static_cast<Micros>(std::llround(s * 1e6));
}

// A closed circuit x = rx cos t + a cos(ka t), y = ry sin t + b sin(kb t), resampled every metre of its
// length.
struct Circuit {
    double length = 0.0;
    std::vector<double> x;
    std::vector<double> y;
    double lat0 = 0.0;
    double lon0 = 0.0;

    std::pair<double, double> at(double s) const {
        double v = std::fmod(s, length);
        if (v < 0.0) {
            v += length;
        }
        const auto n = static_cast<double>(x.size() - 1);
        const double pos = v / length * n;
        const auto i = std::min(static_cast<std::size_t>(pos), x.size() - 2);
        const double f = pos - static_cast<double>(i);
        return {x[i] + f * (x[i + 1] - x[i]), y[i] + f * (y[i + 1] - y[i])};
    }
    double heading(double s) const {
        const auto a = at(s - 0.5);
        const auto b = at(s + 0.5);
        return std::atan2(b.second - a.second, b.first - a.first);
    }
    // 1/m, positive turning left, over a 10 m baseline.
    double curvature(double s) const {
        double d = heading(s + 5.0) - heading(s - 5.0);
        while (d > kPi) {
            d -= 2.0 * kPi;
        }
        while (d < -kPi) {
            d += 2.0 * kPi;
        }
        return d / 10.0;
    }
};

Circuit makeCircuit(double rx, double ry, double a, int ka, double b, int kb, double lat0, double lon0) {
    constexpr int kDense = 40'000;
    std::vector<double> dx(kDense + 1);
    std::vector<double> dy(kDense + 1);
    std::vector<double> cum(kDense + 1, 0.0);
    for (int i = 0; i <= kDense; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / kDense;
        dx[static_cast<std::size_t>(i)] = rx * std::cos(t) + a * std::cos(ka * t);
        dy[static_cast<std::size_t>(i)] = ry * std::sin(t) + b * std::sin(kb * t);
        if (i > 0) {
            const auto k = static_cast<std::size_t>(i);
            cum[k] = cum[k - 1] + std::hypot(dx[k] - dx[k - 1], dy[k] - dy[k - 1]);
        }
    }
    Circuit c;
    c.length = cum.back();
    c.lat0 = lat0;
    c.lon0 = lon0;
    const auto metres = static_cast<std::size_t>(std::floor(c.length));
    c.x.reserve(metres + 2);
    c.y.reserve(metres + 2);
    std::size_t j = 0;
    for (std::size_t m = 0; m <= metres; ++m) {
        const double s = static_cast<double>(m) * c.length / static_cast<double>(metres);
        while (j + 1 < cum.size() - 1 && cum[j + 1] < s) {
            ++j;
        }
        const double span = cum[j + 1] - cum[j];
        const double f = span > 0.0 ? (s - cum[j]) / span : 0.0;
        c.x.push_back(dx[j] + f * (dx[j + 1] - dx[j]));
        c.y.push_back(dy[j] + f * (dy[j + 1] - dy[j]));
    }
    return c;
}

struct Session {
    std::string stem;
    std::string venue;
    std::string driver;
    std::string session;
    std::string vehicle;
    const Circuit* circuit = nullptr;
    double baseLapS = 90.0;
    std::size_t flying = 8;
    double seed = 0.0;
};

std::vector<double> lapSecondsOf(const Session& s) {
    std::vector<double> laps;
    laps.push_back(1.32 * s.baseLapS * (1.0 + 0.01 * std::sin(s.seed)));
    for (std::size_t k = 0; k < s.flying; ++k) {
        const auto kd = static_cast<double>(k);
        laps.push_back(s.baseLapS * (1.0 + 0.012 * std::sin(1.7 * kd + s.seed) + 0.003 * std::cos(0.9 * kd)));
    }
    laps.push_back(1.24 * s.baseLapS);
    return laps;
}

// The car's state at time t of a session.
struct State {
    double t = 0.0;
    std::size_t lap = 0; // 0: the out lap
    double u = 0.0;      // lap fraction
    double v = 0.0;      // m/s
    double accel = 0.0;  // m/s^2 along the path
    double s = 0.0;      // m along the circuit
    double kappa = 0.0;  // 1/m
    double aMax = 1.0;   // the lap's largest acceleration
};

class Kinematics {
public:
    Kinematics(const Circuit& c, std::vector<double> laps, double phase)
        : c_(c), laps_(std::move(laps)), phase_(phase) {
        double t = 0.0;
        for (const double l : laps_) {
            starts_.push_back(t);
            t += l;
        }
        total_ = t;
    }
    double total() const noexcept { return total_; }
    const std::vector<double>& starts() const noexcept { return starts_; }

    State at(double t) const {
        std::size_t k =
            static_cast<std::size_t>(std::upper_bound(starts_.begin(), starts_.end(), t) - starts_.begin());
        k = k == 0 ? 0 : k - 1;
        const double period = laps_[k];
        State st;
        st.t = t;
        st.lap = k;
        st.u = std::clamp((t - starts_[k]) / period, 0.0, 1.0);
        const double w = 2.0 * kPi * st.u;
        const double base = c_.length / period;
        st.v = base * (1.0 + kA * std::sin(w) + kB * std::sin(3.0 * w + phase_));
        st.accel = base * (kA * std::cos(w) * 2.0 * kPi / period +
                           kB * std::cos(3.0 * w + phase_) * 6.0 * kPi / period);
        st.aMax = base * (kA * 2.0 * kPi / period + kB * 6.0 * kPi / period);
        st.s = c_.length * (st.u + kA / (2.0 * kPi) * (1.0 - std::cos(w)) +
                            kB / (6.0 * kPi) * (std::cos(phase_) - std::cos(3.0 * w + phase_)));
        st.kappa = c_.curvature(st.s);
        return st;
    }

private:
    const Circuit& c_;
    std::vector<double> laps_;
    std::vector<double> starts_;
    double total_ = 0.0;
    double phase_ = 0.0;
};

double throttleOf(const State& st) {
    return st.accel >= 0.0 ? std::min(100.0, 30.0 + 70.0 * st.accel / st.aMax) : 4.0;
}
double brakeOf(const State& st) {
    return st.accel < 0.0 ? std::min(100.0, 100.0 * -st.accel / st.aMax) : 0.0;
}
double gearOf(const State& st) {
    return 1.0 + std::min(5.0, std::floor(st.v / 10.0));
}
double rpmOf(const State& st) {
    const double g = gearOf(st);
    return 2'000.0 + std::clamp(st.v - 10.0 * (g - 1.0), 0.0, 10.0) * 600.0;
}

struct ChannelDef {
    std::string name;
    std::string unit;
    unsigned rateHz = 10;
    int dps = 2;
    bool f64 = false;
    std::function<double(const State&)> value;
};

std::vector<ChannelDef> ldChannels(const Circuit& c) {
    std::vector<ChannelDef> d;
    const std::array<const char*, 4> corners{"FL", "FR", "RL", "RR"};
    const std::array<double, 4> wheel{1.002, 0.998, 0.996, 1.0};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const double f = wheel[i];
        d.push_back({std::string("Wheel Speed ") + corners[i], "m/s", 100, 2, false,
                     [f](const State& st) { return st.v * f; }});
    }
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto p = static_cast<double>(i);
        const double load = i < 2 ? 0.002 : 0.0016;
        d.push_back(
            {std::string("Damper Pos ") + corners[i], "mm", 200, 1, false, [p, load](const State& st) {
                 return 25.0 + 4.0 * std::sin(2.0 * kPi * 2.3 * st.t + p) +
                        1.5 * std::sin(2.0 * kPi * 11.0 * st.t + 2.0 * p) + load * st.v * st.v;
             }});
    }
    d.push_back(
        {"Ride Height F", "mm", 100, 1, false, [](const State& st) { return 50.0 - 0.004 * st.v * st.v; }});
    d.push_back(
        {"Ride Height R", "mm", 100, 1, false, [](const State& st) { return 60.0 - 0.003 * st.v * st.v; }});
    d.push_back({"SPEED", "km/h", 50, 1, false, [](const State& st) { return 3.6 * st.v; }});
    d.push_back({"THROTTLE", "%", 50, 1, false, throttleOf});
    d.push_back({"BRAKE", "%", 50, 1, false, brakeOf});
    d.push_back({"STEERANGLE", "deg", 50, 1, false,
                 [](const State& st) { return st.kappa * 2.7 * 14.0 * 180.0 / kPi; }});
    d.push_back(
        {"G Force Lat", "G", 50, 2, false, [](const State& st) { return st.v * st.v * st.kappa / kG; }});
    d.push_back({"G Force Long", "G", 50, 2, false, [](const State& st) { return st.accel / kG; }});
    d.push_back({"RPMS", "rpm", 50, 0, false, rpmOf});
    d.push_back({"GEAR", "", 50, 0, false, gearOf});
    d.push_back({"Brake Press F", "bar g", 50, 1, false, [](const State& st) { return 0.9 * brakeOf(st); }});
    d.push_back({"Brake Press R", "bar g", 50, 1, false, [](const State& st) { return 0.6 * brakeOf(st); }});
    d.push_back({"Oil Temp", "deg C", 10, 1, false,
                 [](const State& st) { return 95.0 + 5.0 * std::sin(st.t / 200.0); }});
    d.push_back({"Water Temp", "deg C", 10, 1, false,
                 [](const State& st) { return 88.0 + 3.0 * std::sin(st.t / 300.0 + 1.0); }});
    d.push_back(
        {"Oil Press", "bar g", 10, 2, false, [](const State& st) { return 1.5 + rpmOf(st) / 2'500.0; }});
    d.push_back(
        {"Fuel Press", "bar g", 10, 2, false, [](const State& st) { return 4.0 + 0.05 * std::sin(st.t); }});
    d.push_back({"Battery Volts", "V", 10, 2, false,
                 [](const State& st) { return 13.6 + 0.1 * std::sin(st.t / 50.0); }});
    d.push_back({"Lambda 1", "lambda", 10, 3, false,
                 [](const State& st) { return 0.86 + 0.06 * (1.0 - throttleOf(st) / 100.0); }});
    d.push_back({"Air Temp", "deg C", 10, 1, false, [](const State& st) { return 22.0 + 0.001 * st.t; }});
    d.push_back({"Exhaust Temp", "deg C", 10, 0, false,
                 [](const State& st) { return 600.0 + 2.0 * throttleOf(st); }});
    d.push_back(
        {"FUEL", "l", 10, 2, false, [](const State& st) { return std::max(0.0, 60.0 - 0.012 * st.t); }});
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto p = static_cast<double>(i);
        d.push_back({std::string("Tyre Temp ") + corners[i], "deg C", 10, 1, false,
                     [p](const State& st) { return 70.0 + 15.0 * (1.0 - std::exp(-st.t / 300.0)) + p; }});
        d.push_back({std::string("Tyre Press ") + corners[i], "bar g", 10, 3, false, [p](const State& st) {
                         return 1.6 + 0.3 * (1.0 - std::exp(-st.t / 300.0)) + 0.01 * p;
                     }});
    }
    const double cosLat = std::cos(c.lat0 * kPi / 180.0);
    d.push_back({"GPS Latitude", "deg", 20, 7, true,
                 [&c](const State& st) { return c.lat0 + c.at(st.s).second / kMetresPerDegree; }});
    d.push_back({"GPS Longitude", "deg", 20, 7, true, [&c, cosLat](const State& st) {
                     return c.lon0 + c.at(st.s).first / (kMetresPerDegree * cosLat);
                 }});
    return d;
}

// Another logger's export: 16 channels at 20 Hz and its lap counter ("Lap", 1 on the out lap), which the
// generic CSV profile's lap source turns into laps.
std::vector<ChannelDef> csvChannels() {
    std::vector<ChannelDef> d;
    d.push_back({"Lap", "", 20, 0, false, [](const State& st) { return static_cast<double>(st.lap + 1); }});
    d.push_back({"SPEED", "km/h", 20, 1, false, [](const State& st) { return 3.6 * st.v; }});
    d.push_back({"THROTTLE", "%", 20, 1, false, throttleOf});
    d.push_back({"BRAKE", "%", 20, 1, false, brakeOf});
    d.push_back({"RPMS", "rpm", 20, 0, false, rpmOf});
    d.push_back({"GEAR", "", 20, 0, false, gearOf});
    d.push_back({"STEERANGLE", "deg", 20, 1, false,
                 [](const State& st) { return st.kappa * 2.7 * 14.0 * 180.0 / kPi; }});
    d.push_back(
        {"G Force Lat", "G", 20, 2, false, [](const State& st) { return st.v * st.v * st.kappa / kG; }});
    d.push_back({"G Force Long", "G", 20, 2, false, [](const State& st) { return st.accel / kG; }});
    d.push_back({"Wheel Speed FL", "m/s", 20, 2, false, [](const State& st) { return st.v * 1.002; }});
    d.push_back({"Wheel Speed FR", "m/s", 20, 2, false, [](const State& st) { return st.v * 0.998; }});
    d.push_back({"Oil Temp", "deg C", 20, 1, false,
                 [](const State& st) { return 96.0 + 4.0 * std::sin(st.t / 180.0); }});
    d.push_back({"Water Temp", "deg C", 20, 1, false,
                 [](const State& st) { return 87.0 + 2.0 * std::sin(st.t / 250.0); }});
    d.push_back({"Battery Volts", "V", 20, 2, false,
                 [](const State& st) { return 13.5 + 0.1 * std::cos(st.t / 40.0); }});
    d.push_back({"Lambda 1", "lambda", 20, 3, false,
                 [](const State& st) { return 0.87 + 0.05 * (1.0 - throttleOf(st) / 100.0); }});
    d.push_back(
        {"FUEL", "l", 20, 2, false, [](const State& st) { return std::max(0.0, 55.0 - 0.011 * st.t); }});
    d.push_back({"Exhaust Temp", "deg C", 20, 0, false,
                 [](const State& st) { return 610.0 + 2.0 * throttleOf(st); }});
    return d;
}

struct Writer {
    const units::UnitRegistry& units;
    formats::ImporterRegistry& registry;
    store::ChunkCache& cache;
};

std::shared_ptr<model::DataSource> buildSource(const Writer& w, const std::string& path,
                                               const Kinematics& kin, const std::vector<ChannelDef>& defs,
                                               std::string_view importer, std::vector<model::Detail> details,
                                               bool beacons) {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view(path));
    src->prov.path = pathFromUtf8(path);
    src->prov.importerId = std::string(importer);
    src->prov.hash = src->id;
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = micros(kin.total());
    // One pass per rate class: the state once per sample, every channel of the class from it.
    std::vector<unsigned> rates;
    for (const ChannelDef& d : defs) {
        if (std::find(rates.begin(), rates.end(), d.rateHz) == rates.end()) {
            rates.push_back(d.rateHz);
        }
    }
    std::vector<std::vector<double>> values(defs.size());
    for (const unsigned rate : rates) {
        const auto n = static_cast<std::size_t>(std::floor(kin.total() * rate));
        for (std::size_t i = 0; i < defs.size(); ++i) {
            if (defs[i].rateHz == rate) {
                values[i].resize(n);
            }
        }
        for (std::size_t k = 0; k < n; ++k) {
            const State st = kin.at(static_cast<double>(k) / rate);
            for (std::size_t i = 0; i < defs.size(); ++i) {
                if (defs[i].rateHz == rate) {
                    values[i][k] = defs[i].value(st);
                }
            }
        }
    }
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const ChannelDef& d = defs[i];
        model::Channel c;
        c.desc.name = d.name;
        c.desc.loggedName = d.name;
        c.desc.id = model::channelIdLogged(src->id, model::kNormalLayer, d.name, importer);
        c.desc.unit = d.unit.empty() ? units::kNoUnit : w.units.find(d.unit).value_or(units::kNoUnit);
        c.desc.quantity = w.units.quantityOf(c.desc.unit);
        c.desc.dtype = d.f64 ? store::Dtype::F64 : store::Dtype::F32;
        c.desc.rate = Rate{d.rateHz, 1};
        c.desc.dps = d.dps;
        store::ColumnOptions co;
        co.cache = &w.cache;
        std::shared_ptr<store::InMemoryProvider> provider;
        if (d.f64) {
            provider = store::InMemoryProvider::fromF64(values[i], {}, "memory:" + d.name);
        } else {
            std::vector<float> f(values[i].size());
            std::transform(values[i].begin(), values[i].end(), f.begin(),
                           [](double v) { return static_cast<float>(v); });
            provider = store::InMemoryProvider::fromF32(f, {}, "memory:" + d.name);
        }
        c.series = store::UniformSeries{Rate{d.rateHz, 1}, 0, store::Column(std::move(provider), co)};
        layer.addChannel(std::move(c));
        values[i] = {};
    }
    layer.markers.push_back(
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived));
    if (beacons) {
        for (std::size_t k = 1; k < kin.starts().size(); ++k) {
            layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, micros(kin.starts()[k]),
                                                            "Beacons", "Beacon", {}, 77, 0,
                                                            model::Provenance::Companion));
        }
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, layer.duration, "", "", "", 0, 0,
                                                    model::Provenance::Derived));
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    src->layers.push_back(std::move(layer));
    src->details = std::move(details);
    src->baseDetails = src->details;
    return src;
}

model::Detail text(std::string id, std::string value) {
    return model::Detail{std::move(id), std::move(value), model::Provenance::Logged, std::nullopt};
}

Result<std::uintmax_t> write(const Writer& w, std::string_view writerId, const std::filesystem::path& out,
                             std::shared_ptr<const model::DataSource> src) {
    formats::IWriter* writer = w.registry.writer(writerId);
    if (writer == nullptr) {
        return fail<std::uintmax_t>(ErrorCode::NotFound, "no writer " + std::string(writerId));
    }
    formats::ExportView view;
    view.source = std::move(src);
    Result<formats::WriteReport> report = writer->write(out, view, {}, {}, {});
    if (!report) {
        return Failure{report.error()};
    }
    return report.value().bytes;
}

// One file to write: a session (or its second version) as .ld + .ldx, or a CSV export.
struct Job {
    std::string kind; // "ld", "legacy-csv", "csv"
    Session session;
    bool variant = false;
    std::string_view writer;
    std::filesystem::path file;
};

constexpr std::string_view kManifestHeader = "esses-soak-logs";

std::string manifestLine(const LogFile& log, const std::filesystem::path& dir) {
    const auto rel = [&dir](const std::filesystem::path& p) {
        return p.empty() ? std::string("-") : pathToUtf8(p.lexically_relative(dir).generic_string());
    };
    return log.kind + "\t" + rel(log.path) + "\t" + rel(log.variant) + "\t" + std::to_string(log.bytes) +
           "\t" + std::to_string(log.laps) + "\n";
}

std::string scaleTag(double scale) {
    return std::to_string(static_cast<long long>(std::llround(scale * 1000.0)));
}

} // namespace

Result<std::vector<LogFile>> writeSaturdayLogs(const std::filesystem::path& dir, double scale) {
    std::error_code ec;
    std::filesystem::create_directories(dir / "variants", ec);
    if (ec) {
        return fail<std::vector<LogFile>>(ErrorCode::Io,
                                          "cannot create " + pathToUtf8(dir) + ": " + ec.message());
    }
    Result<units::UnitRegistry> unitsLoaded =
        units::UnitRegistry::loadFiles(pathFromUtf8(ESSES_TEST_REPO_ROOT) / "data" / "units.json");
    if (!unitsLoaded) {
        return Failure{unitsLoaded.error()};
    }
    const units::UnitRegistry unitTable = std::move(unitsLoaded).value();
    store::ChunkCache cache(std::size_t{2} << 30); // a cache of its own: the soak's runtime starts empty

    // Two venues: about 3.3 km and 2.6 km.
    const Circuit north = makeCircuit(600.0, 380.0, 120.0, 3, 90.0, 2, -33.80, 151.10);
    const Circuit south = makeCircuit(450.0, 300.0, 60.0, 4, 70.0, 3, -34.20, 150.70);
    const auto flying = [scale](std::size_t n) {
        return std::max<std::size_t>(2,
                                     static_cast<std::size_t>(std::llround(static_cast<double>(n) * scale)));
    };
    const std::vector<Session> sessions{
        {"fp1-driver1", "North Park Raceway", "Driver One", "Practice 1", "Car 7", &north, 96.0, flying(8),
         0.3},
        {"fp1-driver2", "North Park Raceway", "Driver Two", "Practice 1", "Car 8", &north, 97.0, flying(7),
         1.1},
        {"fp2-driver1", "North Park Raceway", "Driver One", "Practice 2", "Car 7", &north, 95.5, flying(9),
         2.0},
        {"fp2-driver2", "North Park Raceway", "Driver Two", "Practice 2", "Car 8", &north, 96.2, flying(8),
         2.7},
        {"q-driver1", "North Park Raceway", "Driver One", "Qualifying", "Car 7", &north, 94.8, flying(6),
         3.5},
        {"q-driver2", "North Park Raceway", "Driver Two", "Qualifying", "Car 8", &north, 95.1, flying(6),
         4.2},
        {"test-driver1", "South Loop", "Driver One", "Test", "Car 7", &south, 78.0, flying(10), 5.0},
        {"test-driver2", "South Loop", "Driver Two", "Test", "Car 8", &south, 78.6, flying(9), 5.8},
    };
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        const Session& s = sessions[i];
        jobs.push_back(Job{"ld", s, false, formats::kLdImporterId, dir / pathFromUtf8(s.stem + ".ld")});
        if (i < 2) { // two sessions have a second version, one flying lap longer
            Session v = s;
            v.flying += 1;
            jobs.push_back(
                Job{"ld", v, true, formats::kLdImporterId, dir / "variants" / pathFromUtf8(s.stem + ".ld")});
        }
    }
    // The other logger's two exports, one per CSV writer.
    jobs.push_back(Job{"legacy-csv",
                       Session{"logger-b-race1", "North Park Raceway", "Driver Three", "Race", "Car 21",
                               &north, 97.5, flying(8), 6.4},
                       false, formats::kLegacyCsvImporterId, dir / "logger-b-race1.csv"});
    jobs.push_back(Job{"csv",
                       Session{"logger-b-race2", "North Park Raceway", "Driver Three", "Race", "Car 21",
                               &north, 98.1, flying(7), 7.1},
                       false, formats::kGenericCsvWriterId, dir / "logger-b-race2.csv"});

    // Every file on a pool of its own (each task with its own importer registry; the cache and the unit table
    // are shared, both safe for this).
    TaskPool pool(std::clamp(TaskPool::defaultWorkerCount(), 2U, 8U));
    std::vector<Future<std::uintmax_t>> written;
    written.reserve(jobs.size());
    for (const Job& job : jobs) {
        written.push_back(pool.submit(
            Priority::Background, TaskKey{}, CancelToken{},
            [&job, &unitTable, &cache](const CancelToken&) -> Result<std::uintmax_t> {
                const std::unique_ptr<formats::ImporterRegistry> registry =
                    formats::ImporterRegistry::withBuiltins(&unitTable);
                const Writer w{unitTable, *registry, cache};
                const Session& s = job.session;
                const Kinematics kin(*s.circuit, lapSecondsOf(s), s.seed);
                const bool ld = job.kind == "ld";
                std::vector<model::Detail> details{text("Venue", s.venue), text("Driver", s.driver),
                                                   text("Session", s.session)};
                if (ld) {
                    details.push_back(text("Vehicle Id", s.vehicle));
                    details.push_back(text("Short Comment", "soak"));
                }
                const std::string name = pathToUtf8(job.file.filename());
                return write(w, job.writer, job.file,
                             buildSource(w, name, kin, ld ? ldChannels(*s.circuit) : csvChannels(),
                                         job.writer, std::move(details), ld));
            }));
    }
    std::vector<LogFile> out;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Result<std::uintmax_t>& bytes = written[i].get();
        if (!bytes) {
            return Failure{bytes.error()};
        }
        const Job& job = jobs[i];
        if (job.variant) {
            out.back().variant = job.file;
        } else {
            const std::size_t laps = job.kind == "ld" ? job.session.flying + 2 : 0;
            out.push_back(LogFile{job.file, {}, job.kind, bytes.value(), laps});
        }
    }
    // The manifest last: a folder with it is complete.
    std::string manifest =
        std::string(kManifestHeader) + "\t" + std::to_string(kDataVersion) + "\t" + scaleTag(scale) + "\n";
    for (const LogFile& log : out) {
        manifest += manifestLine(log, dir);
    }
    {
        std::ofstream file(dir / "manifest.txt", std::ios::binary | std::ios::trunc);
        file << manifest;
        file.close();
        if (!file) {
            return fail<std::vector<LogFile>>(ErrorCode::Io,
                                              "cannot write " + pathToUtf8(dir / "manifest.txt"));
        }
    }
    return out;
}

Result<std::vector<LogFile>> readSaturdayLogs(const std::filesystem::path& dir, double scale) {
    const Result<std::string> text = readWholeFile(dir / "manifest.txt", "the soak's log manifest");
    if (!text) {
        return Failure{text.error()};
    }
    std::istringstream in(text.value());
    std::string line;
    std::getline(in, line);
    const std::string header =
        std::string(kManifestHeader) + "\t" + std::to_string(kDataVersion) + "\t" + scaleTag(scale);
    if (line != header) {
        return fail<std::vector<LogFile>>(ErrorCode::Format,
                                          "the soak's log manifest is for other logs: " + line);
    }
    std::vector<LogFile> out;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string kind;
        std::string file;
        std::string variant;
        std::string bytes;
        std::string laps;
        if (!std::getline(fields, kind, '\t') || !std::getline(fields, file, '\t') ||
            !std::getline(fields, variant, '\t') || !std::getline(fields, bytes, '\t') ||
            !std::getline(fields, laps)) {
            return fail<std::vector<LogFile>>(ErrorCode::Format,
                                              "a bad line in the soak's log manifest: " + line);
        }
        LogFile log;
        log.kind = kind;
        log.path = dir / pathFromUtf8(file);
        log.variant = variant == "-" ? std::filesystem::path() : dir / pathFromUtf8(variant);
        log.bytes = std::stoull(bytes);
        log.laps = static_cast<std::size_t>(std::stoull(laps));
        out.push_back(std::move(log));
    }
    return out;
}

Result<std::vector<LogFile>> saturdayLogs(const std::filesystem::path& cacheRoot,
                                          const std::filesystem::path& workDir, double scale) {
    const std::filesystem::path cached =
        cacheRoot / pathFromUtf8("saturday-logs-v" + std::to_string(kDataVersion) + "-x" + scaleTag(scale));
    if (!readSaturdayLogs(cached, scale).ok()) {
        // Written beside it and moved into place: a concurrent run that got there first wins, ours is
        // dropped.
        std::random_device rd;
        std::filesystem::path partial = cached;
        partial += ".partial-" + std::to_string(rd());
        Result<std::vector<LogFile>> made = writeSaturdayLogs(partial, scale);
        if (!made) {
            std::error_code ignored;
            std::filesystem::remove_all(partial, ignored);
            return Failure{made.error()};
        }
        std::error_code ec;
        std::filesystem::remove_all(cached, ec); // an incomplete or older set
        std::filesystem::rename(partial, cached, ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove_all(partial, ignored);
        }
    }
    if (const Result<std::vector<LogFile>> ready = readSaturdayLogs(cached, scale); !ready) {
        return Failure{ready.error()};
    }
    std::error_code ec;
    std::filesystem::remove_all(workDir, ec);
    std::filesystem::copy(
        cached, workDir,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail<std::vector<LogFile>>(ErrorCode::Io, "cannot copy the soak's logs to " +
                                                             pathToUtf8(workDir) + ": " + ec.message());
    }
    return readSaturdayLogs(workDir, scale);
}

} // namespace esses::soak
