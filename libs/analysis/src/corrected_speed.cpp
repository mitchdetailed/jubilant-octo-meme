// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "rule_json.hpp"
#include "series_access.hpp"

#include <esses/analysis/corrected_speed.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/store/memory_provider.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// {"v": <number>, "unit": "<speed unit>"} in m/s: through the registry when it knows the unit (which must be
// a speed), else through the fixed table of rule_json.hpp.
double speedValue(const nlohmann::json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader r(j, pointer);
    const double v = asNumber(r.required("v"), r.child("v"));
    const std::string& unit = asString(r.required("unit"), r.child("unit"));
    r.closed();
    if (const std::optional<units::UnitId> id = units.find(unit)) {
        if (const std::optional<units::UnitId> mps = units.find("m/s");
            mps && units.quantityOf(*id) != units.quantityOf(*mps)) {
            violate(r.child("unit"), "'" + unit + "' is not a speed unit");
        }
        return units.toBase(*id)(v);
    }
    if (const std::optional<double> factor = detail::speedUnitToMps(unit)) {
        return v * *factor;
    }
    violate(r.child("unit"), "unknown unit '" + unit + "'");
}

// A channel name of the settings schema: non-empty, not blank.
const std::string& channelName(const nlohmann::json& j, const std::string& pointer) {
    const std::string& name = asString(j, pointer);
    if (name.find_first_not_of(" \t\r\n") == std::string::npos) {
        violate(pointer, "a channel name is not blank");
    }
    return name;
}

// {"channels": [...]} with between minItems and maxItems names (0: no limit).
std::vector<std::string> channelList(const nlohmann::json& j, const std::string& pointer,
                                     std::size_t minItems, std::size_t maxItems) {
    ObjectReader r(j, pointer);
    std::vector<std::string> out;
    if (const nlohmann::json* p = r.optional("channels")) {
        out = detail::stringArray(*p, r.child("channels"));
        for (std::size_t i = 0; i < out.size(); ++i) {
            channelName((*p)[i], jsonio::pointerIndex(r.child("channels"), i));
        }
        if (out.size() < minItems || (maxItems != 0 && out.size() > maxItems)) {
            violate(r.child("channels"), maxItems == minItems
                                             ? "exactly " + std::to_string(minItems) + " channels"
                                             : "at least " + std::to_string(minItems) + " channel");
        }
    }
    r.closed();
    return out;
}

// One sample of `s` at time t with the Linear policy (store::Resampler rule 13): the sample itself at an
// exact hit, else the interpolation of the bracketing samples, invalid when either is invalid or t lies
// outside the samples.
bool sampleLinear(const UniformSignal& s, Micros t, double& out) noexcept {
    const auto n = static_cast<SampleIdx>(s.size());
    if (n == 0) {
        return false;
    }
    const SampleIdx j = s.rate.indexAt(t, s.t0);
    if (j < 0 || j >= n) {
        return false;
    }
    const auto uj = static_cast<std::size_t>(j);
    const Micros tj = s.rate.timeAt(j, s.t0);
    if (tj == t) {
        if (!s.validAt(uj)) {
            return false;
        }
        out = s.values[uj];
        return true;
    }
    if (j + 1 >= n || !s.validAt(uj) || !s.validAt(uj + 1)) {
        return false;
    }
    const Micros tk = s.rate.timeAt(j + 1, s.t0);
    const double f = static_cast<double>(t - tj) / static_cast<double>(tk - tj);
    out = s.values[uj] + f * (s.values[uj + 1] - s.values[uj]);
    return true;
}

void compactValidity(UniformSignal& s) {
    if (std::all_of(s.valid.begin(), s.valid.end(), [](std::uint8_t v) { return v != 0; })) {
        s.valid.clear();
        s.valid.shrink_to_fit();
    }
}

// fastestWheel over the inputs: one output grid at the highest rate (the first such input's rate, its t0
// moved back by whole periods to the grid point at or before the earliest first sample of any input) up to
// the latest last sample; per sample the maximum of the inputs that are valid there; then the clamp.
Result<UniformSignal> maxOfWheels(std::span<const UniformSignal* const> wheels, const SpeedClamp& clamp) {
    if (wheels.empty()) {
        return Failure{Error{ErrorCode::Invalid, "fastestWheel needs at least one speed signal", {}}};
    }
    std::size_t grid = 0;
    for (std::size_t k = 0; k < wheels.size(); ++k) {
        const UniformSignal& w = *wheels[k];
        if (!w.rate.valid()) {
            return Failure{Error{ErrorCode::Invalid, "fastestWheel: a speed signal has no rate", {}}};
        }
        if (!w.valid.empty() && w.valid.size() != w.values.size()) {
            return Failure{
                Error{ErrorCode::Invalid, "fastestWheel: validity and values differ in length", {}}};
        }
        if (w.rate > wheels[grid]->rate) {
            grid = k;
        }
    }
    UniformSignal out;
    out.rate = wheels[grid]->rate;
    out.t0 = wheels[grid]->t0;
    bool any = false;
    Micros firstT = 0;
    Micros lastT = 0;
    for (const UniformSignal* w : wheels) {
        if (w->size() == 0) {
            continue;
        }
        const Micros t = w->rate.timeAt(static_cast<SampleIdx>(w->size()) - 1, w->t0);
        firstT = any ? std::min(firstT, w->t0) : w->t0;
        lastT = any ? std::max(lastT, t) : t;
        any = true;
    }
    if (any && firstT < out.t0) {
        // A lower-rate wheel starts earlier: its first samples are not dropped (Corr Speed / Corr Dist, and
        // the laps they cover, start with the earliest wheel).
        out.t0 = out.rate.timeAt(out.rate.indexAt(firstT, out.t0), out.t0);
    }
    if (!any || lastT < out.t0) {
        return out;
    }
    // An input on the output grid (same rate, t0 a whole number of periods away) is gathered at index
    // i - shift, without interpolation; any other input is interpolated.
    std::vector<std::optional<SampleIdx>> shift;
    shift.reserve(wheels.size());
    for (const UniformSignal* w : wheels) {
        std::optional<SampleIdx> s;
        if (w->rate == out.rate) {
            const SampleIdx at = out.rate.indexAt(w->t0, out.t0);
            if (out.rate.timeAt(at, out.t0) == w->t0) {
                s = at;
            }
        }
        shift.push_back(s);
    }
    const SampleIdx n = out.rate.indexAt(lastT, out.t0) + 1;
    out.values.resize(static_cast<std::size_t>(n));
    out.valid.resize(static_cast<std::size_t>(n));
    for (SampleIdx i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const Micros t = out.rate.timeAt(i, out.t0);
        bool found = false;
        double best = 0.0;
        for (std::size_t k = 0; k < wheels.size(); ++k) {
            const UniformSignal& w = *wheels[k];
            double v = 0.0;
            bool ok = false;
            if (const std::optional<SampleIdx>& sk = shift[k]) {
                const SampleIdx j = i - *sk;
                const auto uj = static_cast<std::size_t>(j);
                ok = j >= 0 && uj < w.size() && w.validAt(uj);
                v = ok ? w.values[uj] : 0.0;
            } else {
                ok = sampleLinear(w, t, v);
            }
            if (ok && std::isfinite(v) && (!found || v > best)) {
                best = v;
                found = true;
            }
        }
        out.values[ui] = found ? best : kNaN;
        out.valid[ui] = found ? 1 : 0;
    }
    compactValidity(out);
    CorrectedSpeed::applyClamp(out, clamp);
    return out;
}

// A usable speed channel of the layer as a UniformSignal in m/s, or NotFound with the reason.
Result<UniformSignal> loadSpeed(const model::DataLayer& layer, const std::string& name,
                                const units::UnitRegistry* units, const CancelToken& cancel) {
    const model::Channel* c = layer.find(name);
    if (c == nullptr) {
        c = layer.findLogged(name);
    }
    const auto unusable = [&](const std::string& why) {
        Error e{ErrorCode::NotFound, "'" + name + "': " + why, {}};
        e.context.channel = name;
        return Failure{std::move(e)};
    };
    if (c == nullptr) {
        return unusable("no such channel");
    }
    if (c->status == model::ChannelStatus::Error) {
        return unusable("the channel has an error");
    }
    const auto* u = std::get_if<store::UniformSeries>(&c->series);
    if (u == nullptr || !u->rate.valid()) {
        return unusable("timestamped speed channels are not supported yet");
    }
    units::Converter toMps;
    if (units != nullptr && c->desc.unit != units::kNoUnit && units->contains(c->desc.unit)) {
        if (const std::optional<units::UnitId> mps = units->find("m/s");
            mps && units->quantityOf(c->desc.unit) != units->quantityOf(*mps)) {
            return unusable("the channel's unit is not a speed");
        }
        toMps = units->toBase(c->desc.unit);
    }
    UniformSignal s;
    s.rate = u->rate;
    s.t0 = u->t0;
    s.values.reserve(static_cast<std::size_t>(u->size()));
    s.valid.reserve(static_cast<std::size_t>(u->size()));
    auto visited =
        detail::forEachSample(c->series, Priority::Background, cancel, [&](Micros, double v, bool ok) {
            const bool valid = ok && std::isfinite(v);
            s.values.push_back(valid ? toMps(v) : kNaN);
            s.valid.push_back(valid ? 1 : 0);
        });
    if (!visited) {
        return Failure{visited.error()};
    }
    compactValidity(s);
    return s;
}

} // namespace

// ---- settings
// -----------------------------------------------------------------------------------------------

Result<CorrectedSpeedSettings> CorrectedSpeedSettings::fromJson(const JsonDoc& settings,
                                                                const units::UnitRegistry& units) {
    if (settings.isNull()) {
        return CorrectedSpeedSettings{};
    }
    auto parsed = jsonio::parseJson(settings.compact());
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return jsonio::guardJson([&]() -> Result<CorrectedSpeedSettings> {
        const nlohmann::json& j = *parsed;
        if (!j.is_object()) {
            violate("", "the settings are a JSON object");
        }
        CorrectedSpeedSettings s;
        ObjectReader r(j, "");
        if (const nlohmann::json* p = r.optional("method")) {
            const std::string& m = asString(*p, r.child("method"));
            if (m == "fastestWheel") {
                s.method = SpeedMethod::FastestWheel;
            } else if (m == "alternateList") {
                s.method = SpeedMethod::AlternateList;
            } else if (m == "gCorrection") {
                s.method = SpeedMethod::GCorrection;
            } else {
                violate(r.child("method"), "method is fastestWheel, alternateList or gCorrection");
            }
        }
        if (const nlohmann::json* p = r.optional("fastestWheel")) {
            if (auto list = channelList(*p, r.child("fastestWheel"), 2, 2); !list.empty()) {
                s.fastestWheelChannels = std::move(list);
            }
        }
        if (const nlohmann::json* p = r.optional("alternateList")) {
            if (auto list = channelList(*p, r.child("alternateList"), 1, 0); !list.empty()) {
                s.alternateListChannels = std::move(list);
            }
        }
        if (const nlohmann::json* p = r.optional("gCorrection")) {
            ObjectReader g(*p, r.child("gCorrection"));
            if (const nlohmann::json* q = g.optional("speedChannel")) {
                s.gCorrection.speedChannel = channelName(*q, g.child("speedChannel"));
            }
            if (const nlohmann::json* q = g.optional("longGChannel")) {
                s.gCorrection.longGChannel = channelName(*q, g.child("longGChannel"));
            }
            if (const nlohmann::json* q = g.optional("lockThreshold")) {
                s.gCorrection.lockThresholdMps = speedValue(*q, g.child("lockThreshold"), units);
            }
            if (const nlohmann::json* q = g.optional("recoverSeconds")) {
                s.gCorrection.recoverSeconds = asNumber(*q, g.child("recoverSeconds"));
                if (s.gCorrection.recoverSeconds < 0.0) {
                    violate(g.child("recoverSeconds"), "recoverSeconds is not negative");
                }
            }
            g.closed();
        }
        if (const nlohmann::json* p = r.optional("clamp")) {
            ObjectReader c(*p, r.child("clamp"));
            // "min": null means no lower limit; an absent "min" keeps the default 0 km/h.
            if (p->is_object() && p->contains("min") && (*p)["min"].is_null()) {
                s.clamp.minMps.reset();
            }
            if (const nlohmann::json* q = c.optional("min")) {
                s.clamp.minMps = speedValue(*q, c.child("min"), units);
            }
            if (const nlohmann::json* q = c.optional("minInvalidate")) {
                s.clamp.minInvalidate = asBool(*q, c.child("minInvalidate"));
            }
            if (const nlohmann::json* q = c.optional("max")) {
                s.clamp.maxMps = speedValue(*q, c.child("max"), units);
            }
            if (const nlohmann::json* q = c.optional("maxInvalidate")) {
                s.clamp.maxInvalidate = asBool(*q, c.child("maxInvalidate"));
            }
            c.closed();
            if (s.clamp.minMps && s.clamp.maxMps && *s.clamp.minMps > *s.clamp.maxMps) {
                violate(c.child("max"), "max is below min");
            }
        }
        if (const nlohmann::json* p = r.optional("distance")) {
            ObjectReader d(*p, r.child("distance"));
            if (const nlohmann::json* q = d.optional("source")) {
                const std::string& v = asString(*q, d.child("source"));
                if (v == "integrate") {
                    s.distanceSource = DistanceSource::Integrate;
                } else if (v == "logged") {
                    s.distanceSource = DistanceSource::Logged;
                } else {
                    violate(d.child("source"), "source is integrate or logged");
                }
            }
            if (const nlohmann::json* q = d.optional("loggedChannel")) {
                s.loggedDistanceChannel = channelName(*q, d.child("loggedChannel"));
            }
            if (const nlohmann::json* q = d.optional("integration")) {
                const std::string& v = asString(*q, d.child("integration"));
                if (v == "trapezoid") {
                    s.integration = Integration::Trapezoid;
                } else if (v == "rectangle") {
                    s.integration = Integration::Rectangle;
                } else {
                    violate(d.child("integration"), "integration is trapezoid or rectangle");
                }
            }
            d.closed();
        }
        if (const nlohmann::json* p = r.optional("outputs")) {
            ObjectReader o(*p, r.child("outputs"));
            if (const nlohmann::json* q = o.optional("speed")) {
                s.speedOutput = channelName(*q, o.child("speed"));
            }
            if (const nlohmann::json* q = o.optional("distance")) {
                s.distanceOutput = channelName(*q, o.child("distance"));
            }
            o.closed();
            if (s.speedOutput == s.distanceOutput) {
                violate(r.child("outputs"), "the two outputs need distinct names");
            }
        }
        r.closed();
        return s;
    });
}

// ---- Corr Speed
// ---------------------------------------------------------------------------------------------

Result<CorrectedSpeedResult> CorrectedSpeed::compute(const CorrectedSpeedSettings& settings,
                                                     const model::DataLayer& layer,
                                                     const units::UnitRegistry& units,
                                                     const CancelToken& cancel) {
    return compute(settings, layer, &units, cancel);
}

Result<CorrectedSpeedResult> CorrectedSpeed::compute(const CorrectedSpeedSettings& settings,
                                                     const model::DataLayer& layer,
                                                     const units::UnitRegistry* units,
                                                     const CancelToken& cancel) {
    CorrectedSpeedResult out;
    if (settings.method == SpeedMethod::GCorrection) {
        Error e{ErrorCode::Unsupported, "the gCorrection method arrives in M3 (D-044)", {}};
        e.context.detail = "/method";
        return Failure{std::move(e)};
    }
    const auto failed = [](const Result<UniformSignal>& r) {
        return !r && r.error().code != ErrorCode::NotFound; // NotFound: unusable channel, try the next
    };
    if (settings.method == SpeedMethod::FastestWheel) {
        std::vector<UniformSignal> wheels;
        bool usable = !settings.fastestWheelChannels.empty();
        for (const std::string& name : settings.fastestWheelChannels) {
            auto s = loadSpeed(layer, name, units, cancel);
            if (failed(s)) {
                return Failure{s.error()};
            }
            if (!s) {
                usable = false;
                break;
            }
            wheels.push_back(std::move(s).value());
        }
        if (usable) {
            auto speed = fastestWheel(wheels, settings.clamp);
            if (!speed) {
                return Failure{speed.error()};
            }
            for (const std::string& name : settings.fastestWheelChannels) {
                const model::Channel* c = layer.find(name);
                out.inputs.push_back(c != nullptr ? c->desc.name : name);
            }
            out.speedMps = std::move(speed).value();
            return out;
        }
        out.diagnostics.emplace_back("W-CSD-FALLBACK");
    }
    for (const std::string& name : settings.alternateListChannels) {
        auto s = loadSpeed(layer, name, units, cancel);
        if (failed(s)) {
            return Failure{s.error()};
        }
        if (!s) {
            continue;
        }
        out.speedMps = std::move(s).value();
        applyClamp(out.speedMps, settings.clamp);
        const model::Channel* c = layer.find(name);
        out.inputs.push_back(c != nullptr ? c->desc.name : name);
        return out;
    }
    Error e{ErrorCode::NotFound, "no usable speed channel for Corr Speed", {}};
    e.context.detail = "E-CSD-NO-SPEED";
    e.context.layer = layer.name;
    return Failure{std::move(e)};
}

Result<UniformSignal> CorrectedSpeed::fastestWheel(const UniformSignal& a, const UniformSignal& b,
                                                   const SpeedClamp& clamp) {
    const UniformSignal* both[] = {&a, &b};
    return maxOfWheels(both, clamp);
}

Result<UniformSignal> CorrectedSpeed::fastestWheel(std::span<const UniformSignal> wheels,
                                                   const SpeedClamp& clamp) {
    std::vector<const UniformSignal*> ptrs;
    ptrs.reserve(wheels.size());
    for (const UniformSignal& w : wheels) {
        ptrs.push_back(&w);
    }
    return maxOfWheels(ptrs, clamp);
}

void CorrectedSpeed::applyClamp(UniformSignal& speedMps, const SpeedClamp& clamp) {
    if (!clamp.minMps && !clamp.maxMps) {
        return;
    }
    const auto invalidate = [&](std::size_t i) {
        if (speedMps.valid.empty()) {
            speedMps.valid.assign(speedMps.values.size(), 1);
        }
        speedMps.valid[i] = 0;
        speedMps.values[i] = kNaN;
    };
    for (std::size_t i = 0; i < speedMps.values.size(); ++i) {
        if (!speedMps.validAt(i)) {
            continue;
        }
        const double v = speedMps.values[i];
        if (clamp.minMps && v < *clamp.minMps) {
            if (clamp.minInvalidate) {
                invalidate(i);
            } else {
                speedMps.values[i] = *clamp.minMps;
            }
        } else if (clamp.maxMps && v > *clamp.maxMps) {
            if (clamp.maxInvalidate) {
                invalidate(i);
            } else {
                speedMps.values[i] = *clamp.maxMps;
            }
        }
    }
}

// ---- Corr Dist
// ----------------------------------------------------------------------------------------------

Result<UniformSignal> CorrectedDistance::integrate(const UniformSignal& speedMps, Integration method) {
    if (!speedMps.rate.valid()) {
        return Failure{Error{ErrorCode::Invalid, "Corr Dist: the speed signal has no rate", {}}};
    }
    const double dt = static_cast<double>(speedMps.rate.den) / static_cast<double>(speedMps.rate.num);
    UniformSignal out;
    out.rate = speedMps.rate;
    out.t0 = speedMps.t0;
    const std::size_t n = speedMps.size();
    out.values.resize(n);
    double d = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (method == Integration::Trapezoid) {
            if (i > 0 && speedMps.validAt(i - 1) && speedMps.validAt(i)) {
                d += 0.5 * (speedMps.values[i - 1] + speedMps.values[i]) * dt;
            }
        } else if (speedMps.validAt(i)) {
            d += speedMps.values[i] * dt;
        }
        out.values[i] = d;
    }
    return out;
}

Result<UniformSignal> CorrectedDistance::fromLogged(const UniformSignal& lapDistanceM) {
    UniformSignal out;
    out.rate = lapDistanceM.rate;
    out.t0 = lapDistanceM.t0;
    const std::size_t n = lapDistanceM.size();
    out.values.assign(n, 0.0);
    double offset = 0.0;
    double prevRaw = 0.0;
    double held = 0.0;
    bool have = false;
    std::size_t leading = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double raw = lapDistanceM.values[i];
        if (!lapDistanceM.validAt(i) || !std::isfinite(raw)) {
            if (!have) {
                ++leading;
            }
            out.values[i] = held;
            continue;
        }
        if (have && raw < prevRaw - std::max(10.0, prevRaw / 2.0)) {
            offset += prevRaw;
        }
        held = offset + raw;
        out.values[i] = held;
        prevRaw = raw;
        have = true;
    }
    if (!have) {
        return Failure{Error{ErrorCode::NotFound, "Corr Dist: the logged distance has no valid sample", {}}};
    }
    std::fill_n(out.values.begin(), leading, out.values[leading]);
    return out;
}

store::UniformSeries toUniformSeries(const UniformSignal& signal, const store::ColumnOptions& options,
                                     std::string name) {
    std::vector<bool> valid;
    if (!signal.valid.empty()) {
        valid.reserve(signal.valid.size());
        for (const std::uint8_t v : signal.valid) {
            valid.push_back(v != 0);
        }
    }
    store::Column column(store::InMemoryProvider::fromF64(signal.values, std::move(valid), std::move(name)),
                         options);
    return store::UniformSeries{signal.rate, signal.t0, std::move(column)};
}

} // namespace esses::analysis
