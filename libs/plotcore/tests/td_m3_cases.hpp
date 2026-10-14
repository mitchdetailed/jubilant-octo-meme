// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 time_distance worksheet cases (AC-M3-02: "time_distance >= 12 cases including variance, datum,
// offset axis, bands, annotations, status strip"), over the synthetic sources of td_golden_cases.hpp
// decorated with what the M3 elements read: a "Normal" layer holding the laps (Outings:Laps), track sections
// (Outings:Laps:Track Sections:Default with Corners and Straights children), SOL / BCN / EOL markers, device
// events, two status channels (a Bitfield and a Bool), sidecar annotations, and an analysis::DistanceIndex
// per lap beside the known-data lookups (the chain keeps the lookups, so the traces equal the M1 fixture's;
// the variance panel reads the indexes). Every value is rational, so the DrawList JSON goldens of
// tests/golden/drawlist/time_distance/ (plotcore) are identical on every OS; the pixel goldens of
// tests/golden/images/time_distance/ render the same inputs through OffscreenRenderer.
#pragma once

#include "td_golden_cases.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::plot::test {

// The test schemes of td_golden_cases.hpp with the roles the M3 elements draw in (06 section 16).
inline project::Scheme m3LightScheme() {
    project::Scheme s = lightScheme();
    s.roles["rangeMark"] = 0xDCE8F8FFU;
    s.roles["sectionCorner"] = 0xFDE0C2FFU;
    s.roles["sectionStraight"] = 0xDDEBF7FFU;
    s.roles["markerBand"] = 0x5F6368FFU;
    s.roles["annotationMark"] = 0x1A73E8FFU;
    s.roles["deviceEvent"] = 0xE8710AFFU;
    s.roles["varianceZero"] = 0x9AA0A6FFU;
    s.roles["cursorZoom"] = 0x1A73E8FFU;
    return s;
}

inline project::Scheme m3DarkScheme() {
    project::Scheme s = darkScheme();
    s.roles["rangeMark"] = 0x283A55FFU;
    s.roles["sectionCorner"] = 0x5A3A1CFFU;
    s.roles["sectionStraight"] = 0x1E3550FFU;
    s.roles["markerBand"] = 0xBDBDBDFFU;
    s.roles["annotationMark"] = 0x4C9AFFFFU;
    s.roles["deviceEvent"] = 0xFF9F40FFU;
    s.roles["varianceZero"] = 0x70757AFFU;
    s.roles["cursorZoom"] = 0x4C9AFFFFU;
    return s;
}

inline model::MarkerId syntheticMarkerId(const std::string& key) {
    return model::MarkerId{uuidFromHash(hash128(key))};
}

inline model::SourceId syntheticSourceId(char id) {
    model::SourceId s;
    s.lo = static_cast<std::uint64_t>(id);
    s.hi = 0x5EED;
    return s;
}

// The synthetic track sections: lap phases in per mille, name and category.
struct SyntheticSection {
    int from = 0;
    int to = 0;
    const char* name = "";
    const char* category = "";
};
inline constexpr std::array<SyntheticSection, 6> kSyntheticSections{{{0, 120, "Main Straight", "straight"},
                                                                     {120, 320, "T1", "corner"},
                                                                     {320, 420, "Back", "straight"},
                                                                     {420, 600, "T2", "corner"},
                                                                     {600, 720, "Hill", "straight"},
                                                                     {720, 1000, "T3", "corner"}}};

// The decorations of one synthetic source (see the file comment).
struct M3Source {
    char id = 'a';
    std::vector<std::shared_ptr<const analysis::DistanceIndex>> indexes; // per lap; empty: no distance data
    std::vector<model::RangeId> lapIds;
};

inline model::MarkerId lapStartMarker(char id, std::size_t lap) {
    return lap == 0 ? syntheticMarkerId(std::string(1, id) + ":SOL")
                    : syntheticMarkerId(std::string(1, id) + ":BCN" + std::to_string(lap));
}

inline M3Source decorate(const Store& st, SyntheticSource& src, char id, double shift,
                         std::vector<model::Annotation> annotations,
                         const std::vector<std::pair<Micros, std::string>>& deviceEvents) {
    M3Source out;
    out.id = id;
    const Micros outingEnd = src.laps.back().end;
    model::DataLayer layer;
    layer.duration = outingEnd;

    // Markers: SOL, a beacon at every later lap start, EOL, the device events.
    const auto marker = [](model::MarkerClass cls, std::string group, Micros t, model::MarkerId mid,
                           std::string comment) {
        model::Marker m;
        m.id = mid;
        m.cls = cls;
        m.group = std::move(group);
        m.t = t;
        m.comment = std::move(comment);
        return m;
    };
    const std::string sid(1, id);
    layer.markers.push_back(marker(model::MarkerClass::SOL, "", 0, lapStartMarker(id, 0), {}));
    for (std::size_t lap = 1; lap < src.laps.size(); ++lap) {
        layer.markers.push_back(
            marker(model::MarkerClass::BCN, "Beacons", src.laps[lap].start, lapStartMarker(id, lap), {}));
    }
    layer.markers.push_back(
        marker(model::MarkerClass::EOL, "", outingEnd, syntheticMarkerId(sid + ":EOL"), {}));
    for (const auto& [t, text] : deviceEvents) {
        layer.markers.push_back(marker(model::MarkerClass::DeviceEvent, "DeviceEvents", t,
                                       syntheticMarkerId(sid + ":event:" + std::to_string(t)), text));
    }
    std::sort(layer.markers.begin(), layer.markers.end(),
              [](const model::Marker& a, const model::Marker& b) { return a.t < b.t; });

    // Ranges: the outing, its laps and the track sections of every lap.
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    model::Range outing;
    outing.id = model::rangeId("Outings", lapStartMarker(id, 0));
    outing.kind = model::RangeKind::Outing;
    outing.start = 0;
    outing.end = outingEnd;
    outing.name = "Outing 1";
    outings.ranges.push_back(outing);
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    model::RangeGroup sections = model::RangeGroup::make("Outings:Laps:Track Sections:Default");
    model::RangeGroup corners = model::RangeGroup::make("Outings:Laps:Track Sections:Default:Corners");
    model::RangeGroup straights = model::RangeGroup::make("Outings:Laps:Track Sections:Default:Straights");
    for (std::size_t lap = 0; lap < src.laps.size(); ++lap) {
        model::Range r;
        r.id = model::rangeId("Outings:Laps", lapStartMarker(id, lap));
        r.kind = model::RangeKind::Lap;
        r.startMarker = lapStartMarker(id, lap);
        r.start = src.laps[lap].start;
        r.end = src.laps[lap].end;
        r.number = static_cast<int>(lap) + 1;
        r.name = "Lap " + std::to_string(lap + 1);
        out.lapIds.push_back(r.id);
        laps.ranges.push_back(r);
        const Micros duration = r.end - r.start;
        for (const SyntheticSection& s : kSyntheticSections) {
            model::Range section;
            section.id = model::rangeId("Outings:Laps:Track Sections:Default", r.startMarker, s.name);
            section.kind = model::RangeKind::Section;
            section.start = r.start + duration * s.from / 1000;
            section.end = r.start + duration * s.to / 1000;
            section.name = s.name;
            sections.ranges.push_back(section);
            (std::string(s.category) == "corner" ? corners : straights).ranges.push_back(section);
        }
    }
    layer.ranges.setGroup(outings);
    layer.ranges.setGroup(laps);
    layer.ranges.setGroup(model::RangeGroup::make("Outings:Laps:Track Sections"));
    layer.ranges.setGroup(sections);
    layer.ranges.setGroup(corners);
    layer.ranges.setGroup(straights);

    // Status channels at 10 Hz: "Engine Status" (Bitfield: bit 0 through T1's apex, bit 2 on the hill) and
    // "Pit Limiter" (Bool: the first and the last 6 s of the outing).
    const Rate r10{10, 1};
    const auto n10 = static_cast<std::size_t>(r10.indexAt(outingEnd - 1, 0) + 1);
    std::vector<std::int32_t> status(n10);
    std::vector<std::int32_t> limiter(n10);
    for (std::size_t i = 0; i < n10; ++i) {
        const Micros t = r10.timeAt(static_cast<SampleIdx>(i), 0);
        for (const SyntheticLap& lap : src.laps) {
            if (t >= lap.start && t < lap.end) {
                const Micros permille = (t - lap.start) * 1000 / (lap.end - lap.start);
                status[i] =
                    (permille >= 200 && permille < 300 ? 1 : 0) | (permille >= 700 && permille < 760 ? 4 : 0);
            }
        }
        limiter[i] = t < 6 * kSec || t >= outingEnd - 6 * kSec ? 1 : 0;
    }
    const auto statusChannel = [&](const std::string& name, const std::vector<std::int32_t>& v,
                                   store::SampleKind kind) {
        model::Channel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.kind = kind;
        ch.desc.interp = store::Interp::Previous;
        ch.desc.dtype = store::Dtype::I32;
        ch.desc.dps = 0;
        ch.desc.rate = r10;
        ch.series = store::UniformSeries{r10, 0, st.i32(v)};
        return ch;
    };
    layer.addChannel(statusChannel("Engine Status", status, store::SampleKind::Bitfield));
    layer.addChannel(statusChannel("Pit Limiter", limiter, store::SampleKind::Bool));
    src.source->layers.push_back(std::move(layer));
    for (model::Annotation& a : annotations) {
        src.source->sidecar.setAnnotation("Normal", std::move(a));
    }

    // A DistanceIndex per lap from the same trapezoid integral as the lookups (lapsFromSpeed).
    if (!src.pieces.empty()) {
        const Rate r20{20, 1};
        const auto n20 = static_cast<std::size_t>(r20.indexAt(outingEnd - 1, 0) + 1);
        std::vector<Micros> times(n20);
        std::vector<double> speed(n20);
        for (std::size_t i = 0; i < n20; ++i) {
            times[i] = r20.timeAt(static_cast<SampleIdx>(i), 0);
            double p = 0.0;
            for (const SyntheticLap& lap : src.laps) {
                if (times[i] >= lap.start && times[i] < lap.end) {
                    p = static_cast<double>(times[i] - lap.start) / static_cast<double>(lap.end - lap.start);
                }
            }
            speed[i] = static_cast<double>(static_cast<float>(lapSpeed(p, shift)));
        }
        for (std::size_t lap = 0; lap < src.laps.size(); ++lap) {
            std::vector<Micros> t;
            std::vector<double> d;
            double acc = 0.0;
            for (std::size_t i = 0; i < n20; ++i) {
                if (times[i] < src.laps[lap].start || times[i] > src.laps[lap].end) {
                    continue;
                }
                if (!t.empty()) {
                    acc += 0.5 * (speed[i] + speed[i - 1]) * static_cast<double>(times[i] - t.back()) / 1e6;
                }
                t.push_back(times[i]);
                d.push_back(acc);
            }
            analysis::DistanceIndexKey key;
            key.startMarker = lapStartMarker(id, lap);
            key.start = src.laps[lap].start;
            key.end = src.laps[lap].end;
            Result<analysis::DistanceIndex> idx = analysis::DistanceIndex::fromSamples(t, d, key);
            out.indexes.push_back(idx ? std::make_shared<const analysis::DistanceIndex>(std::move(*idx))
                                      : nullptr);
        }
    }
    return out;
}

struct M3Fixture {
    Fixture fx;
    M3Source a;
    M3Source b;
    M3Source c;
    M3Fixture() {
        const auto note = [](const std::string& key, Micros t, const std::string& text) {
            model::Annotation an;
            an.id = uuidFromHash(hash128("annotation:" + key));
            an.tUs = t;
            an.text = text;
            return an;
        };
        a = decorate(fx.store, fx.a, 'a', 0.0,
                     {note("a1", 12 * kSec, "Brake marker early"), note("a2", 101 * kSec, "Kerb strike"),
                      note("a3", 141'500'000, "Traffic")},
                     {{5 * kSec, "Logger start"}, {93 * kSec, "Power cycle"}, {150 * kSec, "Burst trigger"}});
        b = decorate(fx.store, fx.b, 'b', -1.5, {note("b1", 70 * kSec, "Wide at T2")}, {});
        c = decorate(fx.store, fx.c, 'c', 2.0, {}, {});
    }
    const M3Source& decorations(char id) const { return id == 'a' ? a : (id == 'b' ? b : c); }
};

struct M3Case {
    std::string name;
    std::string shows; // what the case pins (the pixel goldens' case description)
    int widthLogical = 440;
    int heightLogical = 300;
    bool dark = false;
    bool focused = false;
    Domain domain = Domain::Time;
    XSpan zoom;
    Micros cursorT = 0;
    std::optional<Micros> datumT;
    bool offsetAxis = false;
    std::string body;
    std::string activeChannel;
    // The override limits of a channel (base units), as the widget fills ResolvedChannel::limitMin/limitMax.
    std::map<std::string, std::pair<double, double>> limits;
    struct SlotSpec {
        char source = 'a';
        std::size_t lap = 0;
        SlotKind kind = SlotKind::Main;
        int number = 0;
        Micros offsetUs = 0;
        double offsetM = 0.0;
    };
    std::vector<SlotSpec> slots;
};

inline std::vector<M3Case> m3Cases(const M3Fixture& m) {
    const std::string speed =
        R"({"name":"Speed","scaleMode":"channel","traces":[{"channel":"Corr Speed","displayUnit":"km/h","dps":1}]})";
    const std::string throttle =
        R"({"name":"Throttle","scaleMode":"manual","scaleMin":{"v":0,"unit":"%"},"scaleMax":{"v":100,"unit":"%"},)"
        R"("traces":[{"channel":"Throttle Pos"}]})";
    const std::string plain =
        R"("showBands":false,"showMarkers":false,"axis":{"showXScrollbar":false,"showYScrollbar":false})";
    const auto lapLength = [&m](char id, std::size_t lap) {
        const SyntheticSource& src = sourceOf(m.fx, id);
        return src.chainFor(src.laps[lap].start)->distAt(src.laps[lap].end);
    };
    std::vector<M3Case> cases;
    {
        M3Case c;
        c.name = "variance_distance";
        c.shows = "F3 in distance mode: dt(d) of Overlay 0 and Overlay 1 against Main in their key colours";
        c.heightLogical = 320;
        c.domain = Domain::Distance;
        c.zoom = {0.0, lapLength('a', 1)};
        c.cursorT = 30 * kSec;
        c.body = R"({"display":{)" + plain + R"(,"variance":{"show":true,"heightFrac":0.25}},"groups":[)" +
                 speed + "," + throttle + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0},
                   {'b', 1, SlotKind::Overlay0, 0, 0, 0.0},
                   {'a', 2, SlotKind::Overlay, 1, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "variance_time";
        c.shows = "F3 in time mode: dd(t) on a manual scale of -40..40 m, blank outside the Main lap";
        c.heightLogical = 320;
        c.zoom = {-4.0 * 1e6, 64.0 * 1e6};
        c.cursorT = 20 * kSec;
        c.body =
            R"({"display":{)" + plain +
            R"(,"variance":{"show":true,"heightFrac":0.3,"autoScale":false,"min":{"v":-40,"unit":"m"},"max":{"v":40,"unit":"m"}}},"groups":[)" +
            speed + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0}, {'b', 1, SlotKind::Overlay0, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "variance_banner";
        c.shows = "F3 with an overlay whose source has no distance data: the variance banner";
        c.domain = Domain::Distance;
        c.zoom = {0.0, 1500.0};
        c.cursorT = 12 * kSec;
        c.body = R"({"display":{)" + plain + R"(,"variance":{"show":true}},"groups":[)" + speed + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0, 0, 0.0}, {'c', 0, SlotKind::Overlay, 1, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "datum_key";
        c.shows =
            "D and SPACE: the key's value@main, value@datum and delta per slot; the horizontal scrollbar";
        c.widthLogical = 560;
        c.zoom = {5.0 * 1e6, 50.0 * 1e6};
        c.cursorT = 30 * kSec;
        c.datumT = 12 * kSec;
        c.body =
            R"({"display":{"showBands":false,"showMarkers":false,"axis":{"showYScrollbar":false}},"groups":[)" +
            speed + "," + throttle + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0}, {'b', 1, SlotKind::Overlay0, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "measurements";
        c.shows = "M: min, max and avg over the visible span per trace and slot";
        c.widthLogical = 600;
        c.heightLogical = 320;
        c.zoom = {10.0 * 1e6, 40.0 * 1e6};
        c.cursorT = 25 * kSec;
        c.body = R"({"display":{)" + plain + R"(,"key":{"stats":true}},"groups":[)" + speed + "," + throttle +
                 "]}";
        c.activeChannel = "Throttle Pos";
        c.slots = {{'a', 0, SlotKind::Main, 0, 0, 0.0}, {'b', 0, SlotKind::Overlay, 1, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "offset_axis_time";
        c.shows = "O in time mode: Main +0.500 s and Overlay 0 -1.200 s on their sub-axes";
        c.zoom = {0.0, 60.0 * 1e6};
        c.cursorT = 21 * kSec;
        c.offsetAxis = true;
        c.body = R"({"display":{)" + plain + R"(},"groups":[)" + speed + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 500'000, 0.0},
                   {'b', 1, SlotKind::Overlay0, 0, -1'200'000, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "offset_axis_distance";
        c.shows = "O in distance mode: Main -12.50 m and Overlay 1 +20.25 m";
        c.domain = Domain::Distance;
        c.zoom = {0.0, lapLength('a', 1)};
        c.cursorT = 30 * kSec;
        c.offsetAxis = true;
        c.body = R"({"display":{)" + plain + R"(},"groups":[)" + speed + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, -12.5}, {'b', 1, SlotKind::Overlay, 1, 0, 20.25}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "bands_sections";
        c.shows = "the range, section and marker bands zoomed out over the neighbouring laps, two sources";
        c.widthLogical = 560;
        c.zoom = {-15.0 * 1e6, 75.0 * 1e6};
        c.cursorT = 30 * kSec;
        c.body = R"({"display":{"axis":{"showXScrollbar":false,"showYScrollbar":false}},"groups":[)" + speed +
                 "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0}, {'b', 1, SlotKind::Overlay, 1, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "annotations_events";
        c.shows = "the marker band: beacons, annotations (with their dotted lines) and device events";
        c.widthLogical = 520;
        c.zoom = {-10.0 * 1e6, 100.0 * 1e6};
        c.cursorT = 40 * kSec;
        c.body =
            R"({"display":{"showBands":false,"axis":{"showXScrollbar":false,"showYScrollbar":false}},"groups":[)" +
            speed + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "status_strip";
        c.shows = "E: the status strip with a Bitfield and a Bool channel, filled where non-zero";
        c.zoom = {0.0, 62.0 * 1e6};
        c.cursorT = 15 * kSec;
        c.body = R"({"display":{)" + plain +
                 R"(,"showStatusStrip":true,"statusChannels":["Engine Status","Pit Limiter"]},"groups":[)" +
                 speed + "," + throttle + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "overlapped";
        c.shows =
            "G: overlapped mode with Lo/Hi regions, handles, the active group's axis and vertical scrollbar";
        c.zoom = {0.0, 60.0 * 1e6};
        c.cursorT = 33 * kSec;
        c.body = R"({"display":{"overlapMode":"overlapped","showBands":false,"showMarkers":false,)"
                 R"("axis":{"showXScrollbar":false}},"groups":[)" +
                 speed.substr(0, speed.size() - 1) + R"(,"lo":0,"hi":0.7},)" +
                 throttle.substr(0, throttle.size() - 1) + R"(,"lo":0.4,"hi":1}]})";
        c.activeChannel = "Throttle Pos";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 0.0}, {'b', 1, SlotKind::Overlay0, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "scale_lines_crosshair";
        c.shows =
            "scale lines zero, min and max (override limits 20..60 m/s on an auto scale) and the crosshair "
            "cursor (Y)";
        c.zoom = {0.0, 60.0 * 1e6};
        c.cursorT = 30 * kSec;
        c.body =
            R"({"display":{)" + plain +
            R"(,"cursorStyle":"crosshair"},"groups":[{"name":"Speed","scaleMode":"auto",)"
            R"("lines":{"min":{"enabled":true,"colour":"#2e7d32"},"max":{"enabled":true,"colour":"#c62828"}},)"
            R"("traces":[{"channel":"Corr Speed","displayUnit":"km/h","dps":1}]},)"
            R"({"name":"Throttle","scaleMode":"manual","scaleMin":{"v":-20,"unit":"%"},"scaleMax":{"v":100,"unit":"%"},)"
            R"("lines":{"zero":{"enabled":true,"colour":"#6a1b9a"}},"traces":[{"channel":"Throttle Pos"}]}]})";
        c.activeChannel = "Corr Speed";
        c.limits["Corr Speed"] = {20.0, 60.0};
        c.slots = {{'a', 0, SlotKind::Main, 0, 0, 0.0}};
        cases.push_back(c);
    }
    {
        M3Case c;
        c.name = "dark_full_anatomy";
        c.shows = "the dark scheme with every M3 element: bands, status strip, variance, offset axis, "
                  "scrollbars, datum";
        c.dark = true;
        c.focused = true;
        c.widthLogical = 600;
        c.heightLogical = 420;
        c.domain = Domain::Distance;
        c.zoom = {-120.0, lapLength('a', 1) + 120.0};
        c.cursorT = 36 * kSec;
        c.datumT = 18 * kSec;
        c.offsetAxis = true;
        c.body = R"({"display":{"showStatusStrip":true,"statusChannels":["Engine Status"],)"
                 R"("variance":{"show":true,"heightFrac":0.25}},"groups":[)" +
                 speed + "," + throttle + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0, 0, 4.0}, {'b', 1, SlotKind::Overlay0, 0, 0, -6.0}};
        cases.push_back(c);
    }
    return cases;
}

inline ComponentBuildInputs m3InputsOf(const M3Fixture& m, const M3Case& mc, float dpr) {
    ComponentBuildInputs in;
    in.spec.type = "time_distance";
    in.spec.caption.show = true;
    in.spec.caption.autoText = true;
    in.spec.activeChannel = mc.activeChannel;
    in.spec.body = model::OpaqueJson::parse(mc.body).value();
    in.view.domain = mc.domain;
    in.view.zoom = mc.zoom;
    in.view.cursorT = mc.cursorT;
    in.view.datumT = mc.datumT;
    in.view.offsetAxis = mc.offsetAxis;
    in.view.version = 1;
    const std::vector<std::string> names = TimeDistanceBody::parse(in.spec.body).value().channelNames();
    for (const M3Case::SlotSpec& s : mc.slots) {
        const SyntheticSource& src = sourceOf(m.fx, s.source);
        const M3Source& deco = m.decorations(s.source);
        SlotData slot = slotOf(src, s.lap, s.kind, s.number, names);
        slot.slot.range.src = syntheticSourceId(s.source);
        slot.slot.range.layer = "Normal";
        slot.slot.range.range = deco.lapIds[s.lap];
        slot.range.offsetUs = s.offsetUs;
        slot.range.offsetM = s.offsetM;
        if (!src.pieces.empty()) {
            std::vector<LapPiece> pieces = src.pieces;
            for (std::size_t i = 0; i < pieces.size() && i < deco.indexes.size(); ++i) {
                pieces[i].idx = deco.indexes[i];
            }
            slot.chain = std::make_shared<const DistanceChain>(std::move(pieces), src.laps[s.lap].start);
        }
        for (ResolvedChannel& rc : slot.channels) {
            if (const auto it = mc.limits.find(rc.name); it != mc.limits.end()) {
                rc.limitMin = it->second.first;
                rc.limitMax = it->second.second;
            }
        }
        in.slots.push_back(std::move(slot));
        const model::RangeRef& ref = in.slots.back().slot.range;
        switch (s.kind) {
        case SlotKind::Main:
            in.selection.main = ref;
            break;
        case SlotKind::Overlay0:
            in.selection.overlay0 = ref;
            break;
        case SlotKind::Overlay:
            in.selection.overlays.push_back(ref);
            break;
        }
    }
    in.scheme = std::make_shared<const project::Scheme>(mc.dark ? m3DarkScheme() : m3LightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(mc.widthLogical) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(mc.heightLogical) * dpr));
    in.layout.dpr = dpr;
    in.focused = mc.focused;
    return in;
}

} // namespace esses::plot::test
