// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The golden cases of the plots work (AC-M3-02; D-198): three cases each of scatter, histogram, outing,
// gauge_numeric and gauge_bar over the synthetic sources of td_golden_cases.hpp (rational data, schemes built
// in code). The gauge cases are also the DrawList JSON goldens of tests/golden/drawlist/gauge_numeric and
// gauge_bar (m3_gauge_tests.cpp, FixedTextMetrics); every case is a pixel golden of
// tests/golden/images/<type>/ (tests/golden/images/tst_golden_plots.cpp, QtTextMetrics with the golden fonts,
// the whole builders including the analysis binners). Header only: shared by the Catch2 and the Qt Test
// executables.
#pragma once

#include "td_golden_cases.hpp"

#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/plotcore.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace esses::plot::test {

struct PlotGoldenCase {
    std::string name;
    std::string type;
    std::string body;
    bool dark = false;
    bool caption = true;
    bool legend = false; // spec.legend.colour (K)
    bool focused = false;
    bool pending = false;
    int width = 420;
    int height = 260;
    Domain domain = Domain::Time;
    XSpan zoom{0.0, 60.0e6};
    Micros cursorT = 20 * kSec;
    std::optional<Micros> datumT;
    std::string activeChannel;
    std::vector<GoldenCase::SlotSpec> slots{{'a', 0, SlotKind::Main, 0}};
};

inline std::vector<PlotGoldenCase> plotGoldenCases(const Fixture& fx) {
    std::vector<PlotGoldenCase> cases;
    const auto add = [&cases](PlotGoldenCase c) { cases.push_back(std::move(c)); };
    // ---- scatter
    {
        PlotGoldenCase c;
        c.name = "scatter_overlay_trend";
        c.type = "scatter";
        c.body = R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Throttle Pos"}],"style":"pointsTrend",)"
                 R"("trend":{"type":"linear"},"cursorStyle":"ring","pointSize":3})";
        c.zoom = {10.0e6, 16.0e6};
        c.cursorT = 12 * kSec;
        c.datumT = 11 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "scatter_tiled_colour";
        c.type = "scatter";
        c.body =
            R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Throttle Pos"},{"yChannel":"Ground Speed"}],)"
            R"("colourChannel":{"channel":"Brake Pres","axis":{"mode":"even","bandMode":"count","bandCount":4,)"
            R"("palette":"rainbow"}},"layout":"tiled","cursorStyle":"cross",)"
            R"("scales":{"x":{"mode":"manual","min":{"v":0,"unit":"km/h"},"max":{"v":250,"unit":"km/h"}}}})";
        c.dark = true;
        c.legend = true;
        c.width = 480;
        c.zoom = {30.0e6, 36.0e6};
        c.cursorT = 33 * kSec;
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "scatter_average_poly";
        c.type = "scatter";
        c.body =
            R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Corr Speed"},{"yChannel":"Ground Speed"}],)"
            R"("layout":"average","style":"trend","trend":{"type":"poly","order":3},)"
            R"("intervalAverage":{"enabled":true,"binWidth":{"v":10,"unit":"km/h"}},"showGrid":false,)"
            R"("cursorStyle":"none"})";
        c.zoom = {0.0, 20.0e6};
        c.cursorT = 5 * kSec;
        c.focused = true;
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        add(c);
    }
    // ---- histogram
    {
        PlotGoldenCase c;
        c.name = "histogram_bar_overlay";
        c.type = "histogram";
        c.body =
            R"({"traces":[{"channel":"Corr Speed"}],"bins":{"mode":"even","bandMode":"width",)"
            R"("dataMin":{"v":50,"unit":"km/h"},"dataMax":{"v":250,"unit":"km/h"},"bandWidth":{"v":25,"unit":"km/h"},)"
            R"("lessMinBand":true,"greaterMaxBand":true},"yMode":"percent","style":"bar"})";
        c.zoom = {0.0, 30.0e6};
        c.cursorT = 14 * kSec;
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "histogram_lines_tiled";
        c.type = "histogram";
        c.body = R"({"traces":[{"channel":"Corr Speed"},{"channel":"Ground Speed"}],"bins":{"mode":"even",)"
                 R"("bandMode":"count","bandCount":12},"yMode":"count","style":"line","layout":"tiled",)"
                 R"("tileCols":1,"condensedAxes":true,"binLabelStep":2})";
        c.dark = true;
        c.height = 320;
        c.cursorT = 40 * kSec;
        c.activeChannel = "Ground Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "histogram_colour_split";
        c.type = "histogram";
        c.body =
            R"({"traces":[{"channel":"Throttle Pos"}],"bins":{"mode":"even","bandMode":"width",)"
            R"("dataMin":{"v":0,"unit":"%"},"dataMax":{"v":100,"unit":"%"},"bandWidth":{"v":10,"unit":"%"},)"
            R"("lessMinBand":false,"greaterMaxBand":false},"yMode":"time","style":"bar",)"
            R"("colourChannel":{"channel":"Gear","axis":{"mode":"even","bandMode":"count","bandCount":3,)"
            R"("dataMin":{"v":1,"unit":""},"dataMax":{"v":6,"unit":""},"palette":"twoColour",)"
            R"("colour1":"#2040ff","colour2":"#ff2020"}}})";
        c.legend = true;
        c.cursorT = 9 * kSec;
        add(c);
    }
    // ---- outing
    const std::string speed = R"({"name":"Speed","traces":[{"channel":"Corr Speed","displayUnit":"km/h"}]})";
    const std::string throttle = R"({"name":"Throttle","scaleMode":"manual","scaleMin":{"v":0,"unit":"%"},)"
                                 R"("scaleMax":{"v":100,"unit":"%"},"traces":[{"channel":"Throttle Pos"}]})";
    {
        PlotGoldenCase c;
        c.name = "outing_time";
        c.type = "outing";
        c.body = R"({"groups":[)" + speed + "," + throttle + "]}";
        c.width = 520;
        c.zoom = {0.0, 61.5e6};
        c.cursorT = 30 * kSec;
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "outing_distance";
        c.type = "outing";
        c.body = R"({"groups":[)" + speed + "]}";
        c.domain = Domain::Distance;
        const auto chain = fx.a.chainFor(fx.a.laps[1].start);
        c.zoom = {0.0, chain->distAt(fx.a.laps[1].end)};
        c.width = 520;
        c.height = 200;
        c.cursorT = 20 * kSec;
        c.slots = {{'a', 1, SlotKind::Main, 0}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "outing_overlays_dark";
        c.type = "outing";
        c.body = R"({"enableOverlays":true,"groups":[)" + speed + "]}";
        c.dark = true;
        c.focused = true;
        c.width = 520;
        c.height = 220;
        c.zoom = {10.0e6, 40.0e6};
        c.cursorT = 25 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay0, 0}};
        add(c);
    }
    // ---- gauge_numeric
    {
        PlotGoldenCase c;
        c.name = "numeric_value_units";
        c.type = "gauge_numeric";
        c.body = R"({"channel":"Corr Speed","displayUnit":"km/h","showUnits":true,"dynamicFont":true})";
        c.width = 240;
        c.height = 140;
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "numeric_reference_text_band";
        c.type = "gauge_numeric";
        c.body =
            R"({"channel":"Ground Speed","displayDps":1,"colourBands":[{"min":{"v":150,"unit":"km/h"},)"
            R"("max":{"v":0,"unit":"km/h"},"open":"above","colour":"#e53935","visible":true}],"bandMode":"text"})";
        c.dark = true;
        c.focused = true;
        c.width = 200;
        c.height = 160;
        c.cursorT = 6 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "numeric_background_fixed_font";
        c.type = "gauge_numeric";
        c.body =
            R"({"channel":"Brake Pres","dynamicFont":false,"font":{"family":"Noto Sans","pointSize":24},)"
            R"("showUnits":false,"colourBands":[{"min":{"v":0,"unit":"bar"},"max":{"v":5,"unit":"bar"},)"
            R"("open":"below","colour":"#43a047","visible":true},{"min":{"v":5,"unit":"bar"},)"
            R"("max":{"v":0,"unit":"bar"},"open":"above","colour":"#fb8c00","visible":true}],)"
            R"("bandMode":"background"})";
        c.caption = false;
        c.pending = true;
        c.width = 180;
        c.height = 100;
        c.cursorT = 9 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        add(c);
    }
    // ---- gauge_bar
    {
        PlotGoldenCase c;
        c.name = "bar_vertical_manual";
        c.type = "gauge_bar";
        c.body =
            R"({"channel":"Brake Pres","displayUnit":"bar","scale":{"mode":"manual","min":{"v":0,"unit":"bar"},)"
            R"("max":{"v":60,"unit":"bar"}},"colourBands":[{"min":{"v":40,"unit":"bar"},"max":{"v":0,"unit":"bar"},)"
            R"("open":"above","colour":"#e53935","visible":true}],"bandLines":"lines"})";
        c.width = 150;
        c.height = 260;
        c.cursorT = 9 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 2, SlotKind::Overlay0, 0}};
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "bar_horizontal_zero_fill";
        c.type = "gauge_bar";
        c.body = R"({"channel":"Ch 03","orientation":"horizontal","originStyle":"zero","scaleDps":1,)"
                 R"("scale":{"mode":"manual","min":{"v":-1,"unit":"V"},"max":{"v":2,"unit":"V"}},)"
                 R"("colourBands":[{"min":{"v":0,"unit":"V"},"max":{"v":1,"unit":"V"},"open":"none",)"
                 R"("colour":"#1e88e5","visible":true},{"min":{"v":1,"unit":"V"},"max":{"v":0,"unit":"V"},)"
                 R"("open":"above","colour":"#e53935","visible":true}],"bandLines":"fill","bandMode":"bar"})";
        c.width = 300;
        c.height = 90;
        c.cursorT = 13 * kSec;
        add(c);
    }
    {
        PlotGoldenCase c;
        c.name = "bar_custom_origin_dark";
        c.type = "gauge_bar";
        c.body = R"({"channel":"Corr Speed","displayUnit":"km/h","originStyle":"custom",)"
                 R"("customOrigin":{"v":120,"unit":"km/h"},"barColour":"#7e57c2","showScale":false})";
        c.dark = true;
        c.focused = true;
        c.width = 110;
        c.height = 240;
        c.cursorT = 15 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        add(c);
    }
    return cases;
}

// The channels a case's builders receive (componentChannelUses order).
inline std::vector<std::string> channelNamesOf(const PlotGoldenCase& c) {
    project::ComponentSpec spec;
    spec.type = c.type;
    spec.body = JsonDoc::parse(c.body).value();
    std::vector<std::string> names;
    for (const ChannelUse& u : componentChannelUses(spec)) {
        names.push_back(u.name);
    }
    return names;
}

// The build inputs of a case at `dpr` (FixedTextMetrics; the pixel goldens replace them with QtTextMetrics).
inline ComponentBuildInputs plotGoldenInputs(const Fixture& fx, const PlotGoldenCase& c, float dpr) {
    ComponentBuildInputs in;
    in.spec.type = c.type;
    in.spec.caption.show = c.caption;
    in.spec.caption.autoText = true;
    in.spec.legend.colour = c.legend;
    in.spec.activeChannel = c.activeChannel;
    in.spec.body = JsonDoc::parse(c.body).value();
    in.view.domain = c.domain;
    in.view.zoom = c.zoom;
    in.view.cursorT = c.cursorT;
    in.view.datumT = c.datumT;
    in.view.version = 1;
    const std::vector<std::string> names = channelNamesOf(c);
    for (const GoldenCase::SlotSpec& s : c.slots) {
        in.slots.push_back(slotOf(sourceOf(fx, s.source), s.lap, s.kind, s.number, names));
        const model::RangeRef& ref = in.slots.back().slot.range;
        if (s.kind == SlotKind::Main) {
            in.selection.main = ref;
        } else if (s.kind == SlotKind::Overlay0) {
            in.selection.overlay0 = ref;
        } else {
            in.selection.overlays.push_back(ref);
        }
    }
    in.scheme = std::make_shared<const project::Scheme>(c.dark ? darkScheme() : lightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(c.width) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(c.height) * dpr));
    in.layout.dpr = dpr;
    in.focused = c.focused;
    return in;
}

// Makes every chunk of the synthetic sources resident (their SummaryPyramids complete), so the scales come
// from the whole columns and the cursor values are found at once (Resampler::sampleAt reads resident chunks).
// True when every column could be read.
inline bool makeResident(const SyntheticSource& src) {
    bool ok = true;
    for (const auto& [name, ch] : src.channels) {
        const store::Column& c = store::seriesValues(ch.series);
        if (c.size() > 0) {
            ok = c.require(0, c.size() - 1, Priority::Visible, CancelToken{}).ok() && ok;
        }
    }
    return ok;
}

} // namespace esses::plot::test
