// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BandAxis (04 section 7.3): the persisted definition (fromJson/toJson) and the generated bands.

#include <esses/analysis/band_axis.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asInt32;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::size_t kMaxGeneratedBands = 10000;
constexpr std::uint32_t kDefaultColour1 = 0x2040FFFF; // 04 section 7.3's example
constexpr std::uint32_t kDefaultColour2 = 0xFF2020FF;
constexpr const char* kColourForm = R"(a colour is "#rrggbb" or "#rrggbbaa")";

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "#rrggbb" (alpha ff) or "#rrggbbaa", either case.
std::uint32_t colourValue(const nlohmann::json& j, const std::string& pointer) {
    const std::string& text = asString(j, pointer);
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
        violate(pointer, kColourForm);
    }
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const int d = hexDigit(text[i]);
        if (d < 0) {
            violate(pointer, kColourForm);
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    return text.size() == 7 ? (v << 8) | 0xFFu : v;
}

// "#rrggbb" when alpha is ff, else "#rrggbbaa", lower case (the schema's "colour").
std::string colourText(std::uint32_t c) {
    constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const int digits = (c & 0xFFu) == 0xFFu ? 6 : 8;
    std::string out = "#";
    for (int i = 0; i < digits; ++i) {
        out.push_back(kHex[(c >> (28 - 4 * i)) & 0xFu]);
    }
    return out;
}

UnitValue unitValue(const nlohmann::json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    UnitValue out;
    out.v = asNumber(r.required("v"), r.child("v"));
    out.unit = asString(r.required("unit"), r.child("unit"));
    r.closed();
    return out;
}

nlohmann::json unitValueJson(const UnitValue& v) {
    return nlohmann::json{{"v", v.v}, {"unit", v.unit}};
}

Failure invalid(std::string message, std::string pointer = {}) {
    Error e{ErrorCode::Invalid, std::move(message), {}};
    e.context.detail = std::move(pointer);
    return Failure{std::move(e)};
}

// A persisted value in `unit` (D-010: the value is in the unit as written). Taken as written without a
// registry, when either spelling is empty or unknown to the registry, or when they are equal; Invalid
// (context.detail = pointer) when the two units do not convert. `delta` converts a width (scale only, so a
// 10 C band is 18 F wide).
Result<double> inUnit(const UnitValue& value, const units::UnitRegistry* registry, std::string_view unit,
                      bool delta, const std::string& pointer) {
    if (registry == nullptr || value.unit.empty() || unit.empty() || value.unit == unit) {
        return value.v;
    }
    const std::optional<units::UnitId> from = registry->find(value.unit);
    const std::optional<units::UnitId> to = registry->find(unit);
    if (!from || !to) {
        return value.v;
    }
    Result<units::Converter> conv = registry->converter(*from, *to);
    if (!conv) {
        return invalid("'" + value.unit + "' does not convert to '" + std::string(unit) + "'", pointer);
    }
    return delta ? conv->a * value.v : (*conv)(value.v);
}

// HSV hue h in degrees, full saturation and value, as 0xRRGGBBff.
std::uint32_t hue(double h) noexcept {
    const double hh = std::fmod(std::max(0.0, h), 360.0) / 60.0;
    const int sector = static_cast<int>(hh);
    const double f = hh - sector;
    const auto byte = [](double v) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    };
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    switch (sector) {
    case 0:
        r = 1.0;
        g = f;
        break;
    case 1:
        r = 1.0 - f;
        g = 1.0;
        break;
    case 2:
        g = 1.0;
        b = f;
        break;
    case 3:
        g = 1.0 - f;
        b = 1.0;
        break;
    case 4:
        r = f;
        b = 1.0;
        break;
    default:
        r = 1.0;
        b = 1.0 - f;
        break;
    }
    return (byte(r) << 24) | (byte(g) << 16) | (byte(b) << 8) | 0xFFu;
}

std::uint32_t blend(std::uint32_t a, std::uint32_t b, double f) noexcept {
    std::uint32_t out = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto ca = static_cast<double>((a >> shift) & 0xFFu);
        const auto cb = static_cast<double>((b >> shift) & 0xFFu);
        out |= static_cast<std::uint32_t>(std::lround(ca + (cb - ca) * f)) << shift;
    }
    return out;
}

// Colour k of n generated bands: "twoColour" blends colour1 into colour2; "rainbow" and every other name
// sweep the hue from blue (240 degrees) to red (0). `reverse` runs the palette backwards.
std::uint32_t paletteColour(const BandAxis& axis, std::size_t k, std::size_t n) noexcept {
    const std::size_t at = axis.reverse ? n - 1 - k : k;
    const double f = n > 1 ? static_cast<double>(at) / static_cast<double>(n - 1) : 0.0;
    if (axis.palette == "twoColour") {
        return blend(axis.colour1.value_or(kDefaultColour1), axis.colour2.value_or(kDefaultColour2), f);
    }
    return hue(240.0 * (1.0 - f));
}

// Edges lo + k * w for k = 0..n; the last one snaps to `snapTo` when it lands within rounding of it.
std::vector<double> evenEdges(double lo, double w, std::size_t n, double snapTo) {
    std::vector<double> edges(n + 1);
    for (std::size_t k = 0; k <= n; ++k) {
        edges[k] = lo + static_cast<double>(k) * w;
    }
    if (std::isfinite(snapTo) &&
        std::abs(edges[n] - snapTo) <= 1e-9 * std::max({std::abs(snapTo), std::abs(lo), w})) {
        edges[n] = snapTo;
    }
    return edges;
}

// The regular bands of an "even" axis over [lo, hi] (see generateBands in band_axis.hpp).
Result<std::vector<Band>> evenBands(const BandAxis& axis, double lo, double hi,
                                    const units::UnitRegistry* units, std::string_view unit) {
    // Width mode without a width falls back to bandCount bands.
    const UnitValue* widthDef = nullptr;
    if (axis.bandMode == BandMode::Width && axis.bandWidth) {
        widthDef = &*axis.bandWidth;
    }
    const bool byWidth = widthDef != nullptr;
    double w = 0.0;
    std::size_t n = 0;
    if (widthDef != nullptr) {
        auto width = inUnit(*widthDef, units, unit, true, "/bandWidth");
        if (!width) {
            return Failure{width.error()};
        }
        w = *width;
        if (!std::isfinite(w) || !(w > 0.0)) {
            return invalid("bandWidth is positive", "/bandWidth");
        }
        const double q = (hi - lo) / w;
        if (!(q < static_cast<double>(kMaxGeneratedBands))) {
            return invalid("the band axis would have more than 10000 bands", "/bandWidth");
        }
        const double slack = 1e-9 * std::max(1.0, q); // (1.1 - 0) / 0.1 is 11.000000000000002
        n = axis.centred ? static_cast<std::size_t>(std::floor(q + slack)) + 1
                         : static_cast<std::size_t>(std::ceil(q - slack));
        n = std::max<std::size_t>(n, 1);
    } else {
        if (axis.bandCount < 1 || static_cast<std::size_t>(axis.bandCount) > kMaxGeneratedBands) {
            return invalid("bandCount is between 1 and 10000", "/bandCount");
        }
        n = static_cast<std::size_t>(axis.bandCount);
        w = (hi - lo) / static_cast<double>(axis.centred && n > 1 ? n - 1 : n);
    }
    std::vector<double> edges;
    if (axis.centred && (byWidth || n > 1)) {
        edges = evenEdges(lo - w / 2.0, w, n, kInf); // band k centred on lo + k * w
    } else if (byWidth) {
        edges = evenEdges(lo, w, n, hi); // full-width bands; the last may end past dataMax
    } else {
        edges.resize(n + 1);
        for (std::size_t k = 0; k < n; ++k) {
            edges[k] = lo + (hi - lo) * static_cast<double>(k) / static_cast<double>(n);
        }
        edges[n] = hi;
    }
    std::vector<Band> out;
    out.reserve(n + 2);
    for (std::size_t k = 0; k < n; ++k) {
        out.push_back(Band{edges[k], edges[k + 1], paletteColour(axis, k, n), false, false});
    }
    return out;
}

} // namespace

Result<BandAxis> BandAxis::fromJson(const JsonDoc& json) {
    const nlohmann::json j = jsonio::fromDoc(json);
    return jsonio::guardJson([&]() -> Result<BandAxis> {
        if (!j.is_object()) {
            violate("", "a band axis is a JSON object");
        }
        BandAxis a;
        ObjectReader r(j, "");
        if (const nlohmann::json* p = r.optional("mode")) {
            const std::string& m = asString(*p, r.child("mode"));
            if (m == "even") {
                a.mode = BandAxisMode::Even;
            } else if (m == "arbitrary") {
                a.mode = BandAxisMode::Arbitrary;
            } else {
                violate(r.child("mode"), "mode is even or arbitrary");
            }
        }
        if (const nlohmann::json* p = r.optional("bandMode")) {
            const std::string& m = asString(*p, r.child("bandMode"));
            if (m == "width") {
                a.bandMode = BandMode::Width;
            } else if (m == "count") {
                a.bandMode = BandMode::Count;
            } else {
                violate(r.child("bandMode"), "bandMode is width or count");
            }
        }
        if (const nlohmann::json* p = r.optional("dataMin")) {
            a.dataMin = unitValue(*p, r.child("dataMin"));
        }
        if (const nlohmann::json* p = r.optional("dataMax")) {
            a.dataMax = unitValue(*p, r.child("dataMax"));
        }
        if (const nlohmann::json* p = r.optional("bandWidth")) {
            a.bandWidth = unitValue(*p, r.child("bandWidth"));
        }
        if (const nlohmann::json* p = r.optional("bandCount")) {
            a.bandCount = asInt32(*p, r.child("bandCount"));
            if (a.bandCount < 1 || a.bandCount > 1000) {
                violate(r.child("bandCount"), "bandCount is between 1 and 1000");
            }
        }
        if (const nlohmann::json* p = r.optional("palette")) {
            a.palette = asString(*p, r.child("palette"));
        }
        if (const nlohmann::json* p = r.optional("colour1")) {
            a.colour1 = colourValue(*p, r.child("colour1"));
        }
        if (const nlohmann::json* p = r.optional("colour2")) {
            a.colour2 = colourValue(*p, r.child("colour2"));
        }
        const std::pair<const char*, bool*> flags[] = {
            {"reverse", &a.reverse},         {"centred", &a.centred},
            {"lessMinBand", &a.lessMinBand}, {"greaterMaxBand", &a.greaterMaxBand},
            {"continuous", &a.continuous},
        };
        for (const auto& [name, target] : flags) {
            if (const nlohmann::json* p = r.optional(name)) {
                *target = asBool(*p, r.child(name));
            }
        }
        if (const nlohmann::json* p = r.optional("bands")) {
            const std::string pointer = r.child("bands");
            if (!p->is_array()) {
                violate(pointer, "bands is an array");
            }
            for (std::size_t i = 0; i < p->size(); ++i) {
                ObjectReader b((*p)[i], jsonio::pointerIndex(pointer, i));
                BandDef def;
                def.min = unitValue(b.required("min"), b.child("min"));
                def.max = unitValue(b.required("max"), b.child("max"));
                if (const nlohmann::json* c = b.optional("colour")) {
                    def.colour = colourValue(*c, b.child("colour"));
                }
                b.closed();
                a.bands.push_back(std::move(def));
            }
        }
        if (const nlohmann::json unknown = r.unread(); !unknown.empty()) {
            auto doc = jsonio::toDoc(unknown);
            if (!doc) {
                return Failure{doc.error()};
            }
            a.rest = std::move(doc).value();
        }
        return a;
    });
}

JsonDoc BandAxis::toJson() const {
    nlohmann::json j = rest.isObject() ? jsonio::fromDoc(rest) : nlohmann::json::object();
    j["mode"] = mode == BandAxisMode::Even ? "even" : "arbitrary";
    j["bandMode"] = bandMode == BandMode::Width ? "width" : "count";
    const std::pair<const char*, const std::optional<UnitValue>*> values[] = {
        {"dataMin", &dataMin}, {"dataMax", &dataMax}, {"bandWidth", &bandWidth}};
    for (const auto& [name, value] : values) {
        if (const std::optional<UnitValue>& v = *value) {
            j[name] = unitValueJson(*v);
        } else {
            j.erase(name);
        }
    }
    j["bandCount"] = bandCount;
    j["palette"] = palette;
    const std::pair<const char*, const std::optional<std::uint32_t>*> colours[] = {{"colour1", &colour1},
                                                                                   {"colour2", &colour2}};
    for (const auto& [name, value] : colours) {
        if (const std::optional<std::uint32_t>& c = *value) {
            j[name] = colourText(*c);
        } else {
            j.erase(name);
        }
    }
    j["reverse"] = reverse;
    j["centred"] = centred;
    j["lessMinBand"] = lessMinBand;
    j["greaterMaxBand"] = greaterMaxBand;
    j["continuous"] = continuous;
    if (bands.empty()) {
        j.erase("bands");
    } else {
        nlohmann::json list = nlohmann::json::array();
        for (const BandDef& b : bands) {
            list.push_back({{"min", unitValueJson(b.min)},
                            {"max", unitValueJson(b.max)},
                            {"colour", colourText(b.colour)}});
        }
        j["bands"] = std::move(list);
    }
    auto doc = jsonio::toDoc(j);
    return doc ? std::move(doc).value() : JsonDoc{};
}

Result<std::vector<Band>> generateBands(const BandAxis& axis, double autoMin, double autoMax,
                                        const units::UnitRegistry* units, std::string_view unit) {
    std::vector<Band> out;
    if (axis.mode == BandAxisMode::Arbitrary) {
        for (std::size_t i = 0; i < axis.bands.size(); ++i) {
            const std::string pointer = "/bands/" + std::to_string(i);
            auto lo = inUnit(axis.bands[i].min, units, unit, false, pointer + "/min");
            if (!lo) {
                return Failure{lo.error()};
            }
            auto hi = inUnit(axis.bands[i].max, units, unit, false, pointer + "/max");
            if (!hi) {
                return Failure{hi.error()};
            }
            out.push_back(Band{*lo, *hi, axis.bands[i].colour, false, false});
        }
    } else {
        double lo = autoMin;
        double hi = autoMax;
        if (axis.dataMin) {
            auto v = inUnit(*axis.dataMin, units, unit, false, "/dataMin");
            if (!v) {
                return Failure{v.error()};
            }
            lo = *v;
        }
        if (axis.dataMax) {
            auto v = inUnit(*axis.dataMax, units, unit, false, "/dataMax");
            if (!v) {
                return Failure{v.error()};
            }
            hi = *v;
        }
        if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
            return invalid("the band axis needs dataMin < dataMax", "/dataMax");
        }
        auto even = evenBands(axis, lo, hi, units, unit);
        if (!even) {
            return Failure{even.error()};
        }
        out = std::move(even).value();
    }
    if (out.empty()) {
        return invalid("the band axis has no bands", "/bands");
    }
    for (std::size_t k = 0; k < out.size(); ++k) {
        const bool increasing = std::isfinite(out[k].lo) && std::isfinite(out[k].hi) &&
                                out[k].lo < out[k].hi && (k == 0 || out[k - 1].hi <= out[k].lo);
        if (!increasing) {
            return invalid("the bands are not increasing",
                           axis.mode == BandAxisMode::Arbitrary ? "/bands/" + std::to_string(k) : "");
        }
    }
    if (axis.lessMinBand) {
        out.insert(out.begin(), Band{-kInf, out.front().lo, out.front().colour, true, false});
    }
    if (axis.greaterMaxBand) {
        out.push_back(Band{out.back().hi, kInf, out.back().colour, false, true});
    }
    return out;
}

} // namespace esses::analysis
