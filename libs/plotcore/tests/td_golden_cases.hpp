// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The eight M1 worksheet cases of AC-M1-04 as ComponentBuildInputs over synthetic, deterministic data: single
// trace, 20 traces, like-units group, two panels, distance mode with an overlay, dark scheme, time mode with
// Overlay 0 and two overlays (D-017 colours, single-colour Main), and a source without distance data (the
// banner). Each case renders at DPR 1 and 2. The DrawList JSON goldens of tests/golden/drawlist/time_distance
// compare against these inputs on every OS; the pixel goldens of esses_qtrender can render the same inputs.
//
// Determinism across platforms: every sample comes from rational arithmetic (+ - * / and floor, no libm
// transcendental), the text metrics are FixedTextMetrics and the schemes are built here, so the DrawLists are
// identical wherever IEEE double arithmetic is (plotcore builds with -ffp-contract=off on GCC/Clang).
#pragma once

#include "plotcore_test_support.hpp"

#include <esses/model/source.hpp>
#include <esses/plotcore/plotcore.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace esses::plot::test {

inline constexpr Micros kSec = 1'000'000;

inline project::Scheme lightScheme() {
    project::Scheme s;
    s.name = "Test Light";
    s.background = "light";
    s.roles = {{"cptBackground", 0xFFFFFFFFU},
               {"keyBackground", 0xF7F7F8FFU},
               {"cptForeground", 0x202124FFU},
               {"cptForeground2", 0x6B6F75FFU},
               {"cptFrame", 0xA0A4A8FFU},
               {"dataGrid", 0xE4E6E8FFU},
               {"cursorMain", 0xD93025FFU},
               {"cursorDatum", 0xB8860BFFU},
               {"cptCaptionBackground", 0xE8EAEDFFU},
               {"cptCaptionForeground", 0x202124FFU},
               {"cptFocusBorder", 0x1A73E8FFU},
               {"pendingHatch", 0xC8CBD0FFU},
               {"mainSingleColour", 0x202124FFU},
               {"overlay0", 0x8A8F96FFU}};
    s.channelPalette = {0xD32F2FFFU, 0x1565C0FFU, 0x2E7D32FFU, 0xEF6C00FFU, 0x6A1B9AFFU, 0x00838FFFU,
                        0xC2185BFFU, 0x558B2FFFU, 0x4527A0FFU, 0x9E7700FFU, 0x00695CFFU, 0x5D4037FFU};
    s.overlayPalette = {0x0288D1FFU, 0x7CB342FFU, 0xF57C00FFU, 0x8E24AAFFU, 0x00897BFFU,
                        0xD81B60FFU, 0xFBC02DFFU, 0x546E7AFFU, 0x6D4C41FFU};
    return s;
}

inline project::Scheme darkScheme() {
    project::Scheme s;
    s.name = "Test Dark";
    s.background = "dark";
    s.roles = {{"cptBackground", 0x141517FFU},
               {"keyBackground", 0x1A1B1EFFU},
               {"cptForeground", 0xE6E6E6FFU},
               {"cptForeground2", 0x9AA0A6FFU},
               {"cptFrame", 0x3A3D41FFU},
               {"dataGrid", 0x33363AFFU},
               {"cursorMain", 0xFF3B30FFU},
               {"cursorDatum", 0xFFD60AFFU},
               {"cptCaptionBackground", 0x2B2D31FFU},
               {"cptCaptionForeground", 0xE6E6E6FFU},
               {"cptFocusBorder", 0x4C9AFFFFU},
               {"pendingHatch", 0x3A3D41FFU},
               {"mainSingleColour", 0xFFFFFFFFU},
               {"overlay0", 0xBDBDBDFFU}};
    s.channelPalette = {0xFF5252FFU, 0x40C4FFFFU, 0x69F0AEFFU, 0xFFD740FFU, 0xEA80FCFFU, 0xFFAB40FFU,
                        0x18FFFFFFU, 0xB2FF59FFU, 0xFF80ABFFU, 0x8C9EFFFFU, 0xCCFF90FFU, 0xFFE57FFFU};
    s.overlayPalette = {0x4FC3F7FFU, 0xAED581FFU, 0xFFB74DFFU, 0xBA68C8FFU, 0x4DB6ACFFU,
                        0xF06292FFU, 0xFFF176FFU, 0x90A4AEFFU, 0xA1887FFFU};
    return s;
}

// ---- synthetic sources -----------------------------------------------------------------------------------

struct SyntheticLap {
    Micros start = 0;
    Micros end = 0;
};

struct SyntheticChannel {
    model::ChannelDescriptor desc;
    store::Series series;
    units::Converter toDisplay;
    std::string unit;
};

struct SyntheticSource {
    std::shared_ptr<model::DataSource> source;
    std::vector<SyntheticLap> laps;
    std::map<std::string, SyntheticChannel> channels;
    std::shared_ptr<const DistanceChain> chainFor(Micros origin) const {
        return pieces.empty() ? nullptr : std::make_shared<const DistanceChain>(pieces, origin);
    }
    OutingGeom outing() const { return OutingGeom{laps.front().start, laps.back().end}; }
    std::vector<LapPiece> pieces; // empty: no distance data
};

// Speed through a lap (m/s) at lap phase p in [0, 1): a piecewise-linear circuit profile.
inline double lapSpeed(double p, double shift) {
    static constexpr std::array<double, 9> kPhase{0.0, 0.12, 0.18, 0.32, 0.42, 0.60, 0.72, 0.90, 1.0};
    static constexpr std::array<double, 9> kSpeed{50.0, 62.0, 25.0, 55.0, 18.0, 66.0, 30.0, 60.0, 50.0};
    p = p - std::floor(p);
    std::size_t i = 1;
    while (i + 1 < kPhase.size() && p > kPhase[i]) {
        ++i;
    }
    const double f = (p - kPhase[i - 1]) / (kPhase[i] - kPhase[i - 1]);
    return kSpeed[i - 1] + f * (kSpeed[i] - kSpeed[i - 1]) + shift;
}

inline double triangle(double x) {
    const double f = x - std::floor(x);
    return 1.0 - std::abs(2.0 * f - 1.0);
}

inline SyntheticSource makeSource(const Store& st, const std::string& file, std::vector<SyntheticLap> laps,
                                  double shift, bool withDistance) {
    SyntheticSource src;
    src.source = std::make_shared<model::DataSource>();
    src.source->prov.path = file;
    src.laps = std::move(laps);
    const Micros outingEnd = src.laps.back().end;
    const auto phaseAt = [&](Micros t) {
        for (const SyntheticLap& lap : src.laps) {
            if (t >= lap.start && t < lap.end) {
                return static_cast<double>(t - lap.start) / static_cast<double>(lap.end - lap.start);
            }
        }
        return 0.0;
    };
    const auto addChannel = [&](const std::string& name, Rate rate, store::Column column,
                                units::Converter conv, std::string unit, int dps, store::SampleKind kind,
                                store::Interp interp) {
        SyntheticChannel ch;
        ch.desc.name = name;
        ch.desc.loggedName = name;
        ch.desc.kind = kind;
        ch.desc.interp = interp;
        ch.desc.dps = dps;
        ch.desc.rate = rate;
        ch.desc.dtype = column.dtype();
        ch.series = store::UniformSeries{rate, 0, std::move(column)};
        ch.toDisplay = conv;
        ch.unit = std::move(unit);
        src.channels[name] = std::move(ch);
    };
    const Rate r20{20, 1};
    const Rate r10{10, 1};
    const auto n20 = static_cast<std::size_t>(r20.indexAt(outingEnd - 1, 0) + 1);
    const auto n10 = static_cast<std::size_t>(r10.indexAt(outingEnd - 1, 0) + 1);
    std::vector<float> speed(n20);
    std::vector<float> ground(n20);
    std::vector<float> throttle(n20);
    std::vector<float> brake(n20);
    std::vector<Micros> times(n20);
    std::vector<double> speedD(n20);
    for (std::size_t i = 0; i < n20; ++i) {
        const Micros t = r20.timeAt(static_cast<SampleIdx>(i), 0);
        const double p = phaseAt(t);
        const double v = lapSpeed(p, shift);
        const double ahead = lapSpeed(p + 0.02, shift);
        times[i] = t;
        speedD[i] = static_cast<double>(static_cast<float>(v));
        speed[i] = static_cast<float>(v);
        ground[i] = static_cast<float>(v * 1.01 + 0.3);
        throttle[i] = static_cast<float>(std::clamp((ahead - v) * 9.0 + 55.0, 0.0, 100.0));
        brake[i] = static_cast<float>(std::clamp((v - ahead) * 4.0e5, 0.0, 6.0e6));
    }
    std::vector<std::int32_t> gear(n10);
    for (std::size_t i = 0; i < n10; ++i) {
        const Micros t = r10.timeAt(static_cast<SampleIdx>(i), 0);
        gear[i] =
            std::min<std::int32_t>(6, 1 + static_cast<std::int32_t>(lapSpeed(phaseAt(t), shift) / 12.0));
    }
    addChannel("Corr Speed", r20, st.f32(speed), {3.6, 0.0}, "km/h", 1, store::SampleKind::Real,
               store::Interp::Linear);
    addChannel("Ground Speed", r20, st.f32(ground), {3.6, 0.0}, "km/h", 1, store::SampleKind::Real,
               store::Interp::Linear);
    addChannel("Throttle Pos", r20, st.f32(throttle), {}, "%", 0, store::SampleKind::Real,
               store::Interp::Linear);
    addChannel("Brake Pres", r20, st.f32(brake), {1e-5, 0.0}, "bar", 1, store::SampleKind::Real,
               store::Interp::Linear);
    addChannel("Gear", r10, st.i32(gear), {}, "", 0, store::SampleKind::Integer, store::Interp::Previous);
    for (int k = 1; k <= 20; ++k) {
        std::vector<float> v(n10);
        const double period = static_cast<double>(3 + k);
        const double amp = 0.5 + 0.25 * static_cast<double>(k % 5);
        for (std::size_t i = 0; i < n10; ++i) {
            const double t = static_cast<double>(r10.timeAt(static_cast<SampleIdx>(i), 0)) / 1e6;
            v[i] =
                static_cast<float>(amp * triangle(t / period + 0.05 * shift) + 0.1 * static_cast<double>(k));
        }
        const std::string name = std::string("Ch ") + (k < 10 ? "0" : "") + std::to_string(k);
        addChannel(name, r10, st.f32(v), {}, "V", 2, store::SampleKind::Real, store::Interp::Linear);
    }
    if (withDistance) {
        std::vector<Micros> bounds;
        for (const SyntheticLap& lap : src.laps) {
            bounds.push_back(lap.start);
        }
        bounds.push_back(outingEnd);
        src.pieces = lapsFromSpeed(times, speedD, bounds);
    }
    return src;
}

struct Fixture {
    Store store;
    SyntheticSource a;
    SyntheticSource b;
    SyntheticSource c;

    Fixture()
        : a(makeSource(store, "synthetic_a.ld",
                       {{0, 60 * kSec}, {60 * kSec, 121'500'000}, {121'500'000, 180 * kSec}}, 0.0, true)),
          b(makeSource(store, "synthetic_b.ld",
                       {{0, 61 * kSec}, {61 * kSec, 120 * kSec}, {120 * kSec, 182 * kSec}}, -1.5, true)),
          c(makeSource(store, "synthetic_c.ld", {{0, 59 * kSec}, {59 * kSec, 120 * kSec}}, 2.0, false)) {}
};

// One visible slot: `lap` of `src` with the component's channels resolved in channelNames order.
inline SlotData slotOf(const SyntheticSource& src, std::size_t lap, SlotKind kind, int number,
                       const std::vector<std::string>& channels) {
    SlotData s;
    s.slot.kind = kind;
    s.slot.number = number;
    s.slot.range.name = "Lap " + std::to_string(lap + 1);
    s.slot.range.groupPath = "Outings:Laps";
    s.source = src.source;
    s.range = RangeGeom{src.laps[lap].start, src.laps[lap].end};
    s.outing = src.outing();
    s.chain = src.chainFor(src.laps[lap].start);
    for (const std::string& name : channels) {
        ResolvedChannel rc;
        rc.name = name;
        if (const auto it = src.channels.find(name); it != src.channels.end()) {
            rc.descriptor = it->second.desc;
            rc.series = it->second.series;
            rc.toDisplay = it->second.toDisplay;
            rc.displayUnit = it->second.unit;
            rc.dps = it->second.desc.dps;
        }
        s.channels.push_back(std::move(rc));
    }
    return s;
}

struct GoldenCase {
    std::string name;
    int widthLogical = 400;
    int heightLogical = 260;
    bool dark = false;
    bool pending = false;
    bool focused = false;
    Domain domain = Domain::Time;
    XSpan zoom;
    Micros cursorT = 0;
    std::optional<Micros> datumT;
    std::string body;
    std::string activeChannel;
    bool singleColourMain = false;
    // (source, lap, kind, number): Main first.
    struct SlotSpec {
        char source = 'a';
        std::size_t lap = 0;
        SlotKind kind = SlotKind::Main;
        int number = 0;
    };
    std::vector<SlotSpec> slots;
};

inline const SyntheticSource& sourceOf(const Fixture& fx, char id) {
    return id == 'a' ? fx.a : (id == 'b' ? fx.b : fx.c);
}

// The M1 cases draw what M1 drew: the M3 bands and scrollbars, shown by default since M3 (04 section 8.14),
// are switched off in their bodies, so their goldens did not change with the M3 builder.
inline std::string m1Display() {
    return R"("showBands":false,"showMarkers":false,"axis":{"showXScrollbar":false,"showYScrollbar":false})";
}

inline std::vector<GoldenCase> goldenCases(const Fixture& fx) {
    const std::string speedGroup =
        R"({"name":"Speed","scaleMode":"channel","traces":[{"channel":"Corr Speed","displayUnit":"km/h","dps":1}]})";
    const std::string throttleGroup =
        R"({"name":"Throttle","scaleMode":"manual","scaleMin":{"v":0,"unit":"%"},"scaleMax":{"v":100,"unit":"%"},)"
        R"("traces":[{"channel":"Throttle Pos"}]})";
    std::vector<GoldenCase> cases;
    {
        GoldenCase c;
        c.name = "single_trace";
        c.zoom = {0.0, 60.0 * 1e6};
        c.cursorT = 20 * kSec;
        c.body = R"({"display":{"traceStyle":"line","lineWidth":1,"showGrid":true,)" + m1Display() +
                 R"(},"groups":[)" + speedGroup + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "twenty_traces";
        c.widthLogical = 420;
        c.heightLogical = 420;
        c.zoom = {10.0 * 1e6, 30.0 * 1e6};
        c.cursorT = 15 * kSec;
        std::string groups;
        for (int g = 0; g < 4; ++g) {
            groups += std::string(g == 0 ? "" : ",") + R"({"name":"Group )" + std::to_string(g + 1) +
                      R"(","scaleMode":"channel","traces":[)";
            for (int t = 0; t < 5; ++t) {
                const int k = g * 5 + t + 1;
                groups += std::string(t == 0 ? "" : ",") + R"({"channel":"Ch )" + (k < 10 ? "0" : "") +
                          std::to_string(k) + R"("})";
            }
            groups += "]}";
        }
        c.body = R"({"display":{)" + m1Display() + R"(},"groups":[)" + groups + "]}";
        c.activeChannel = "Ch 07";
        c.slots = {{'a', 0, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "like_units_group";
        c.zoom = {0.0, 60.0 * 1e6};
        c.cursorT = 31 * kSec;
        c.body = R"({"display":{)" + m1Display() +
                 R"(},"groups":[{"name":"Speed","scaleMode":"channel","traces":[{"channel":"Corr Speed"},)"
                 R"({"channel":"Ground Speed"}]}]})";
        c.activeChannel = "Ground Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "two_panels";
        c.zoom = {5.0 * 1e6, 45.0 * 1e6};
        c.cursorT = 22'500'000;
        c.body = R"({"display":{)" + m1Display() + R"(},"groups":[)" +
                 speedGroup.substr(0, speedGroup.size() - 1) + R"(,"edges":[0,0.6]},)" +
                 throttleGroup.substr(0, throttleGroup.size() - 1) + R"(,"edges":[0.6,1]}]})";
        c.activeChannel = "Throttle Pos";
        c.slots = {{'a', 1, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "distance_overlay";
        c.domain = Domain::Distance;
        const auto chain = fx.a.chainFor(fx.a.laps[1].start);
        c.zoom = {0.0, chain->distAt(fx.a.laps[1].end)};
        c.cursorT = 30 * kSec;
        c.body =
            R"({"display":{)" + m1Display() + R"(},"groups":[)" + speedGroup + "," + throttleGroup + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "dark_scheme";
        c.dark = true;
        c.zoom = {5.0 * 1e6, 45.0 * 1e6};
        c.cursorT = 22'500'000;
        c.datumT = 10 * kSec;
        c.body = R"({"display":{"key":{"pos":"right"},)" + m1Display() + R"(},"groups":[)" + speedGroup +
                 "," + throttleGroup + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "time_overlays";
        c.zoom = {0.0, 62.0 * 1e6};
        c.cursorT = 40 * kSec;
        c.singleColourMain = true;
        c.body =
            R"({"display":{)" + m1Display() + R"(},"groups":[)" + speedGroup +
            R"(,{"name":"Gear","scaleMode":"manual","scaleMin":0,"scaleMax":7,"traces":[{"channel":"Gear"}]}]})";
        c.activeChannel = "Gear";
        c.slots = {{'a', 0, SlotKind::Main, 0},
                   {'b', 0, SlotKind::Overlay0, 0},
                   {'a', 2, SlotKind::Overlay, 1},
                   {'b', 2, SlotKind::Overlay, 2}};
        cases.push_back(c);
    }
    {
        GoldenCase c;
        c.name = "no_distance_banner";
        c.domain = Domain::Distance;
        c.pending = true;
        c.focused = true;
        c.zoom = {0.0, 1500.0};
        c.cursorT = 12 * kSec;
        c.body = R"({"display":{)" + m1Display() + R"(},"groups":[)" + speedGroup + "]}";
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'c', 0, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    return cases;
}

inline ComponentBuildInputs inputsOf(const Fixture& fx, const GoldenCase& gc, float dpr) {
    ComponentBuildInputs in;
    in.spec.type = "time_distance";
    in.spec.caption.show = true;
    in.spec.caption.autoText = true;
    in.spec.activeChannel = gc.activeChannel;
    in.spec.body = model::OpaqueJson::parse(gc.body).value();
    in.view.domain = gc.domain;
    in.view.zoom = gc.zoom;
    in.view.cursorT = gc.cursorT;
    in.view.datumT = gc.datumT;
    in.view.version = 1;
    in.selection.singleColourMain = gc.singleColourMain;
    const std::vector<std::string> names = TimeDistanceBody::parse(in.spec.body).value().channelNames();
    for (const GoldenCase::SlotSpec& s : gc.slots) {
        const SyntheticSource& src = sourceOf(fx, s.source);
        in.slots.push_back(slotOf(src, s.lap, s.kind, s.number, names));
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
    in.scheme = std::make_shared<const project::Scheme>(gc.dark ? darkScheme() : lightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(gc.widthLogical) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(gc.heightLogical) * dpr));
    in.layout.dpr = dpr;
    in.focused = gc.focused;
    return in;
}

} // namespace esses::plot::test
