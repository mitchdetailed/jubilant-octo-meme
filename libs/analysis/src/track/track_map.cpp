// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMap (track-1, 02 section 6.9). Owner: the track work (src/track/).

#include "track_path.hpp"

#include <esses/analysis/track_map.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::pointerIndex;
using jsonio::violate;
using nlohmann::json;

constexpr double kEpsM = 1e-6; // tolerance of the section bounds and of the 10 m minimum

// A {v, unit} length in metres.
double readLength(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    const double v = asNumber(r.required("v"), r.child("v"));
    const std::string& unit = asString(r.required("unit"), r.child("unit"));
    r.closed();
    const std::optional<double> scale = trackpath::lengthUnitToMetres(unit);
    if (!scale) {
        violate(r.child("unit"), "'" + unit + "' is not a length unit (m, km, cm, mm, mi, ft, yd, in)");
    }
    return v * *scale;
}

json lengthJson(double metres) {
    return json{{"v", metres}, {"unit", "m"}};
}

std::vector<TrackPoint> readLine(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        violate(pointer, "a line is an array of [x, y, cumDist] rows");
    }
    std::vector<TrackPoint> out;
    out.reserve(j.size());
    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string p = pointerIndex(pointer, i);
        const json& row = j[i];
        if (!row.is_array() || row.size() != 3) {
            violate(p, "a line row is [x, y, cumDist]");
        }
        out.push_back(TrackPoint{asNumber(row[0], pointerIndex(p, 0)), asNumber(row[1], pointerIndex(p, 1)),
                                 asNumber(row[2], pointerIndex(p, 2))});
    }
    return out;
}

json lineJson(const std::vector<TrackPoint>& line) {
    json out = json::array();
    for (const TrackPoint& p : line) {
        out.push_back(json::array({p.x, p.y, p.dist}));
    }
    return out;
}

std::string_view directionName(TrackDirection d) noexcept {
    return d == TrackDirection::CounterClockwise ? "ccw" : "cw";
}

std::string_view methodName(TrackMethod m) noexcept {
    switch (m) {
    case TrackMethod::Gps:
        return "gps";
    case TrackMethod::SpeedLatG:
        return "speedLatG";
    case TrackMethod::None:
        break;
    }
    return "";
}

Failure invalidAt(std::string pointer, std::string message) {
    Error e{ErrorCode::Invalid, std::move(message) + " (at '" + pointer + "')", {}};
    e.context.detail = std::move(pointer);
    return Failure{std::move(e)};
}

bool finite(double v) noexcept {
    return std::isfinite(v);
}

Result<void> checkLine(const std::vector<TrackPoint>& line, std::string_view name, bool ordered) {
    const std::string base = "/lines/" + std::string(name);
    for (std::size_t i = 0; i < line.size(); ++i) {
        const TrackPoint& p = line[i];
        if (!finite(p.x) || !finite(p.y) || !finite(p.dist)) {
            return invalidAt(pointerIndex(base, i), "a line point is finite");
        }
        if (ordered && i > 0 && p.dist < line[i - 1].dist) {
            return invalidAt(pointerIndex(base, i), "the centreline is ordered by cumulative distance");
        }
    }
    return ok();
}

} // namespace

Result<TrackMap> TrackMap::fromJson(const JsonDoc& document) {
    const json j = jsonio::fromDoc(document);
    return jsonio::guardJson([&]() -> Result<TrackMap> {
        if (!j.is_object()) {
            violate("", "a track-1 document is a JSON object");
        }
        if (j.contains("$schema") && !j["$schema"].is_null()) {
            jsonio::checkSchemaId(j, "track", 1);
        }
        ObjectReader r(j, "");
        r.skip("$schema");
        r.skip("schemaVersion");
        TrackMap map;
        if (const json* p = r.optional("name")) {
            map.name = asString(*p, r.child("name"));
        }
        if (const json* p = r.optional("venue")) {
            map.venue = asString(*p, r.child("venue"));
        }
        if (const json* p = r.optional("origin")) {
            ObjectReader o(*p, r.child("origin"));
            map.origin.lat = asNumber(o.required("lat"), o.child("lat"));
            map.origin.lon = asNumber(o.required("lon"), o.child("lon"));
            if (const json* a = o.optional("alt")) {
                map.origin.alt = asNumber(*a, o.child("alt"));
            }
            o.closed();
        }
        if (const json* p = r.optional("rotationDeg")) {
            map.rotationDeg = asNumber(*p, r.child("rotationDeg"));
        }
        if (const json* p = r.optional("direction")) {
            const std::string& d = asString(*p, r.child("direction"));
            if (d == "cw") {
                map.direction = TrackDirection::Clockwise;
            } else if (d == "ccw") {
                map.direction = TrackDirection::CounterClockwise;
            } else {
                violate(r.child("direction"), R"(the direction is "cw" or "ccw")");
            }
        }
        if (const json* p = r.optional("closed")) {
            map.closed = asBool(*p, r.child("closed"));
        }
        if (const json* p = r.optional("generatedFrom")) {
            ObjectReader g(*p, r.child("generatedFrom"));
            if (const json* m = g.optional("method")) {
                const std::string& method = asString(*m, g.child("method"));
                if (method == "gps") {
                    map.generatedFrom.method = TrackMethod::Gps;
                } else if (method == "speedLatG") {
                    map.generatedFrom.method = TrackMethod::SpeedLatG;
                } else {
                    violate(g.child("method"), R"(the method is "gps" or "speedLatG")");
                }
            }
            if (const json* s = g.optional("source")) {
                map.generatedFrom.source = asString(*s, g.child("source"));
            }
            if (const json* l = g.optional("lap")) {
                map.generatedFrom.lap = asString(*l, g.child("lap"));
            }
            g.closed();
        }
        {
            ObjectReader l(r.required("lines"), r.child("lines"));
            map.centre = readLine(l.required("centre"), l.child("centre"));
            if (const json* p = l.optional("inner")) {
                map.inner = readLine(*p, l.child("inner"));
            }
            if (const json* p = l.optional("outer")) {
                map.outer = readLine(*p, l.child("outer"));
            }
            l.closed();
        }
        if (const json* p = r.optional("length")) {
            map.lengthM = readLength(*p, r.child("length"));
        } else {
            map.lengthM = map.centre.empty() ? 0.0 : map.centre.back().dist;
        }
        if (const json* p = r.optional("sections")) {
            const std::string sp = r.child("sections");
            if (!p->is_array()) {
                violate(sp, "sections is an array");
            }
            for (std::size_t i = 0; i < p->size(); ++i) {
                ObjectReader s((*p)[i], pointerIndex(sp, i));
                TrackSection section;
                section.id = asString(s.required("id"), s.child("id"));
                section.name = asString(s.required("name"), s.child("name"));
                if (const json* v = s.optional("set")) {
                    section.set = asString(*v, s.child("set"));
                }
                section.startM = readLength(s.required("startDist"), s.child("startDist"));
                section.endM = readLength(s.required("endDist"), s.child("endDist"));
                if (const json* v = s.optional("category")) {
                    section.category = asString(*v, s.child("category"));
                }
                if (const json* v = s.optional("userDefined")) {
                    section.userDefined = asBool(*v, s.child("userDefined"));
                }
                s.closed();
                map.sections.push_back(std::move(section));
            }
        }
        const json unknown = r.unread();
        if (!unknown.empty()) {
            auto doc = jsonio::toDoc(unknown);
            if (!doc) {
                violate("", doc.error().message);
            }
            map.rest = std::move(doc).value();
        }
        return map;
    });
}

JsonDoc TrackMap::toJson() const {
    json j = json::object();
    if (rest.isObject()) {
        j = jsonio::fromDoc(rest);
    }
    j["$schema"] = "urn:esses:track-1";
    j["schemaVersion"] = 1;
    j["name"] = name;
    j["venue"] = venue;
    j["origin"] = json{{"lat", origin.lat}, {"lon", origin.lon}, {"alt", origin.alt}};
    j["rotationDeg"] = rotationDeg;
    j["direction"] = directionName(direction);
    j["closed"] = closed;
    j["length"] = lengthJson(lengthM);
    if (generatedFrom.method != TrackMethod::None || !generatedFrom.source.empty() ||
        !generatedFrom.lap.empty()) {
        json g = json{{"source", generatedFrom.source}, {"lap", generatedFrom.lap}};
        if (generatedFrom.method != TrackMethod::None) {
            g["method"] = methodName(generatedFrom.method);
        }
        j["generatedFrom"] = std::move(g);
    }
    j["lines"] = json{{"centre", lineJson(centre)}, {"inner", lineJson(inner)}, {"outer", lineJson(outer)}};
    json sectionsJson = json::array();
    for (const TrackSection& s : sections) {
        sectionsJson.push_back(json{{"id", s.id},
                                    {"set", s.set},
                                    {"name", s.name},
                                    {"startDist", lengthJson(s.startM)},
                                    {"endDist", lengthJson(s.endM)},
                                    {"category", s.category},
                                    {"userDefined", s.userDefined}});
    }
    j["sections"] = std::move(sectionsJson);
    auto doc = jsonio::toDoc(j);
    return doc ? std::move(doc).value() : JsonDoc{};
}

Result<void> TrackMap::validate() const {
    if (!finite(lengthM) || lengthM < 0.0) {
        return invalidAt("/length", "the track length is a finite, non-negative distance");
    }
    if (!finite(origin.lat) || !finite(origin.lon) || !finite(origin.alt) || std::abs(origin.lat) > 90.0 ||
        std::abs(origin.lon) > 180.0) {
        return invalidAt("/origin", "the origin is a WGS-84 latitude, longitude and altitude");
    }
    if (!finite(rotationDeg) || std::abs(rotationDeg) > 360.0) {
        return invalidAt("/rotationDeg", "the rotation lies between -360 and 360 degrees");
    }
    if (auto c = checkLine(centre, "centre", true); !c) {
        return c;
    }
    if (auto c = checkLine(inner, "inner", false); !c) {
        return c;
    }
    if (auto c = checkLine(outer, "outer", false); !c) {
        return c;
    }
    std::set<std::string, std::less<>> ids;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const TrackSection& s = sections[i];
        const std::string p = pointerIndex("/sections", i);
        if (s.id.empty()) {
            return invalidAt(p + "/id", "a section has an id");
        }
        if (!ids.insert(s.id).second) {
            return invalidAt(p + "/id", "the section id '" + s.id + "' is used twice");
        }
        if (s.name.empty()) {
            return invalidAt(p + "/name", "a section has a name");
        }
        if (s.set.empty()) {
            return invalidAt(p + "/set", "a section belongs to a named set");
        }
        if (s.category.empty()) {
            return invalidAt(p + "/category", "a section has a category");
        }
        if (!finite(s.startM) || s.startM < -kEpsM) {
            return invalidAt(p + "/startDist", "the section '" + s.name + "' starts before the start line");
        }
        if (!finite(s.endM) || s.endM > lengthM + kEpsM) {
            return invalidAt(p + "/endDist", "the section '" + s.name + "' ends after the track length");
        }
        if (s.lengthM() < kMinSectionM - kEpsM) {
            return invalidAt(p, "the section '" + s.name + "' is shorter than 10 m");
        }
    }
    return ok();
}

TrackPoint TrackMap::pointAt(double distM) const noexcept {
    if (centre.empty() || !finite(distM)) {
        return TrackPoint{0.0, 0.0, finite(distM) ? distM : 0.0};
    }
    if (centre.size() == 1) {
        return TrackPoint{centre.front().x, centre.front().y, distM};
    }
    double d = distM;
    if (closed && lengthM > 0.0) {
        d = std::fmod(d, lengthM);
        if (d < 0.0) {
            d += lengthM;
        }
    } else {
        d = std::clamp(d, centre.front().dist, centre.back().dist);
    }
    if (d >= centre.back().dist) {
        const TrackPoint& last = centre.back();
        if (closed && lengthM > last.dist) {
            const TrackPoint& first = centre.front();
            const double t = (d - last.dist) / (lengthM - last.dist);
            return TrackPoint{last.x + t * (first.x - last.x), last.y + t * (first.y - last.y), d};
        }
        return TrackPoint{last.x, last.y, d};
    }
    if (d <= centre.front().dist) {
        return TrackPoint{centre.front().x, centre.front().y, d};
    }
    const auto hi = std::upper_bound(centre.begin(), centre.end(), d,
                                     [](double v, const TrackPoint& p) { return v < p.dist; });
    const TrackPoint& b = *hi;
    const TrackPoint& a = *(hi - 1);
    const double span = b.dist - a.dist;
    const double t = span > 0.0 ? (d - a.dist) / span : 0.0;
    return TrackPoint{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), d};
}

std::vector<std::string> TrackMap::sectionSets() const {
    std::vector<std::string> sets;
    const bool hasDefault = std::any_of(sections.begin(), sections.end(),
                                        [](const TrackSection& s) { return s.set == kDefaultSet; });
    if (hasDefault) {
        sets.emplace_back(kDefaultSet);
    }
    for (const TrackSection& s : sections) {
        if (std::find(sets.begin(), sets.end(), s.set) == sets.end()) {
            sets.push_back(s.set);
        }
    }
    return sets;
}

} // namespace esses::analysis
