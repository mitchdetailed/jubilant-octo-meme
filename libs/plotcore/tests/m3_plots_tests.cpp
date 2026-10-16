// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterGeometry (04 section 10) and HistogramGeometry (04 section 11). The scenes (points, trends, interval
// averages, density grids, histogram values) are computed here by brute force from the synthetic sources of
// td_golden_cases.hpp, so the DrawList JSON goldens (tests/golden/drawlist/scatter, histogram) pin the frame
// and the drawing independently of esses_analysis's binners; the bands are given in the test (generateBands
// is the analysis side's). The end-to-end builds run through ScatterBinner / HistogramBinner and skip their
// checks while those are M3 stubs (the Static build then fails with Unsupported, which OffscreenRenderer
// draws as "Render failed").

#include "geometry/plot_scenes.hpp"
#include "m3_golden_support.hpp"

#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/plotcore/scatter_geometry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

constexpr Micros kSec = test::kSec;
constexpr Micros kPeriod = 50'000; // the 20 Hz channels of the synthetic sources
constexpr double kInf = std::numeric_limits<double>::infinity();

std::vector<std::string> namesOf(const std::vector<ChannelUse>& uses) {
    std::vector<std::string> out;
    out.reserve(uses.size());
    for (const ChannelUse& u : uses) {
        out.push_back(u.name);
    }
    return out;
}

struct PlotCase {
    std::string name;
    std::string type;
    std::string body;
    bool dark = false;
    bool legend = false;
    int width = 420;
    int height = 260;
    XSpan zoom;
    Micros cursorT = 0;
    std::optional<Micros> datumT;
    std::string activeChannel;
    std::vector<test::GoldenCase::SlotSpec> slots;
};

ComponentBuildInputs plotInputs(const test::Fixture& fx, const PlotCase& c,
                                const std::vector<std::string>& names, float dpr) {
    ComponentBuildInputs in;
    in.spec.type = c.type;
    in.spec.caption.show = true;
    in.spec.caption.autoText = true;
    in.spec.legend.colour = c.legend;
    in.spec.activeChannel = c.activeChannel;
    in.spec.body = JsonDoc::parse(c.body).value();
    in.view.domain = Domain::Time;
    in.view.zoom = c.zoom;
    in.view.cursorT = c.cursorT;
    in.view.datumT = c.datumT;
    in.view.version = 1;
    for (const test::GoldenCase::SlotSpec& s : c.slots) {
        in.slots.push_back(test::slotOf(test::sourceOf(fx, s.source), s.lap, s.kind, s.number, names));
        const model::RangeRef& ref = in.slots.back().slot.range;
        if (s.kind == SlotKind::Main) {
            in.selection.main = ref;
        } else if (s.kind == SlotKind::Overlay0) {
            in.selection.overlay0 = ref;
        } else {
            in.selection.overlays.push_back(ref);
        }
    }
    in.scheme = std::make_shared<const project::Scheme>(c.dark ? test::darkScheme() : test::lightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(c.width) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(c.height) * dpr));
    in.layout.dpr = dpr;
    return in;
}

// The display value of a 20 Hz channel at sample i (NaN when missing).
double valueAt(const SlotData& slot, const std::string& channel, SampleIdx i) {
    for (const ResolvedChannel& rc : slot.channels) {
        if (rc.name == channel && rc.series) {
            return rc.toDisplay(store::seriesValues(*rc.series).at(i));
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// The 20 Hz sample indices of a slot's visible span.
std::pair<SampleIdx, SampleIdx> spanOf(const ComponentBuildInputs& in, const SlotData& slot) {
    const Micros t0 = slot.range.start + static_cast<Micros>(in.view.zoom.a);
    const Micros t1 = slot.range.start + static_cast<Micros>(in.view.zoom.b);
    return {(t0 + kPeriod - 1) / kPeriod, (t1 + kPeriod - 1) / kPeriod};
}

std::string fixed4(double v) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 4);
    return {buf.data(), r.ptr};
}

// Least squares y = c0 + c1 x (normal equations; fine for a test reference).
analysis::TrendResult linearFit(const std::vector<double>& x, const std::vector<double>& y) {
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    const auto n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    analysis::TrendResult r;
    const double c1 = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    r.coefficients = {(sy - c1 * sx) / n, c1};
    r.samples = x.size();
    return r;
}

// ---- scatter

std::vector<PlotCase> scatterCases() {
    std::vector<PlotCase> cases;
    {
        PlotCase c;
        c.name = "scatter_overlay_trend";
        c.type = "scatter";
        c.body = R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Throttle Pos"}],"style":"pointsTrend",)"
                 R"("cursorStyle":"ring","pointSize":3})";
        c.zoom = {10.0e6, 16.0e6};
        c.cursorT = 12 * kSec;
        c.datumT = 11 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    {
        PlotCase c;
        c.name = "scatter_tiled_colour";
        c.type = "scatter";
        c.body =
            R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Throttle Pos"},{"yChannel":"Ground Speed"}],)"
            R"("colourChannel":{"channel":"Brake Pres"},"layout":"tiled","cursorStyle":"cross",)"
            R"("scales":{"x":{"mode":"manual","min":{"v":0,"unit":"km/h"},"max":{"v":250,"unit":"km/h"}}}})";
        c.dark = true;
        c.legend = true;
        c.width = 480;
        c.zoom = {30.0e6, 36.0e6};
        c.cursorT = 33 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        PlotCase c;
        c.name = "scatter_density_interval";
        c.type = "scatter";
        c.body =
            R"({"xChannel":"Corr Speed","traces":[{"yChannel":"Corr Speed"},{"yChannel":"Ground Speed"}],)"
            R"("layout":"average","intervalAverage":{"enabled":true,"binWidth":{"v":10,"unit":"km/h"}},)"
            R"("showGrid":false,"cursorStyle":"none"})";
        c.zoom = {0.0, 20.0e6};
        c.cursorT = 5 * kSec;
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        cases.push_back(c);
    }
    return cases;
}

// The reference scene of a scatter case: the sets in draw order (overlays first) with points sampled on the
// 20 Hz grid of the visible span.
plot::detail::ScatterScene scatterScene(const ComponentBuildInputs& in, const ScatterBody& body,
                                        const plot::detail::ScatterFrame& frame, const std::string& name) {
    plot::detail::ScatterScene scene;
    for (std::size_t k = in.slots.size(); k-- > 0;) {
        const SlotData& slot = in.slots[k];
        const auto [i0, i1] = spanOf(in, slot);
        const auto pushSet = [&](int trace, int tile, const std::vector<std::string>& ys) {
            plot::detail::ScatterSet set;
            set.trace = trace;
            set.slot = k;
            set.tile = tile;
            for (SampleIdx i = i0; i < i1; ++i) {
                const double x = valueAt(slot, body.xChannel, i);
                double sum = 0.0;
                for (const std::string& y : ys) {
                    sum += valueAt(slot, y, i);
                }
                const double y = sum / static_cast<double>(ys.size());
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    continue;
                }
                set.series.x.push_back(x);
                set.series.y.push_back(y);
                set.series.t.push_back(i * kPeriod);
                if (!body.colourChannel.empty()) {
                    set.series.z.push_back(valueAt(slot, body.colourChannel, i));
                }
            }
            scene.sets.push_back(std::move(set));
        };
        if (frame.average) {
            std::vector<std::string> ys;
            ys.reserve(frame.traces.size());
            for (const int t : frame.traces) {
                ys.push_back(body.traces[static_cast<std::size_t>(t)].yChannel);
            }
            pushSet(-1, 0, ys);
        } else {
            for (std::size_t n = 0; n < frame.traces.size(); ++n) {
                const int t = frame.traces[n];
                pushSet(t, frame.tiles.size() > 1 ? static_cast<int>(n) : 0,
                        {body.traces[static_cast<std::size_t>(t)].yChannel});
            }
        }
    }
    for (plot::detail::ScatterSet& set : scene.sets) {
        if (name == "scatter_overlay_trend") {
            set.trend = linearFit(set.series.x, set.series.y);
            if (set.slot == 0) {
                set.equation =
                    "y = " + fixed4(set.trend->coefficients[1]) + "x + " + fixed4(set.trend->coefficients[0]);
            }
        }
        if (name == "scatter_density_interval") {
            std::map<double, std::pair<double, int>> bins;
            for (std::size_t i = 0; i < set.series.x.size(); ++i) {
                auto& b = bins[std::floor(set.series.x[i] / 10.0) * 10.0];
                b.first += set.series.y[i];
                ++b.second;
            }
            analysis::IntervalAverage iv;
            iv.binLo.reserve(bins.size());
            iv.mean.reserve(bins.size());
            for (const auto& [lo, acc] : bins) {
                iv.binLo.push_back(lo);
                iv.mean.push_back(acc.first / acc.second);
            }
            set.interval = iv;
            // An 8 x 8 density grid over the tile's scales with a fixed pattern.
            const plot::detail::PlotTile& tile = frame.tiles.front();
            set.density.nx = 8;
            set.density.ny = 8;
            set.density.x0 = tile.x.vMin;
            set.density.x1 = tile.x.vMax;
            set.density.y0 = tile.y.vMin;
            set.density.y1 = tile.y.vMax;
            for (int r = 0; r < 8; ++r) {
                for (int c = 0; c < 8; ++c) {
                    set.density.counts.push_back(
                        static_cast<std::uint32_t>(r == c ? 40 + 5 * r : ((r + c) % 3) * 7));
                }
            }
        }
    }
    scene.density = name == "scatter_density_interval";
    return scene;
}

std::vector<analysis::Band> brakeBands() {
    // Brake Pres in bar: four bands with an open band above.
    return {analysis::Band{0.0, 5.0, 0x2E7D32FFU, false, false},
            analysis::Band{5.0, 15.0, 0xF9A825FFU, false, false},
            analysis::Band{15.0, 30.0, 0xEF6C00FFU, false, false},
            analysis::Band{30.0, kInf, 0xC62828FFU, false, true}};
}

struct ScatterRun {
    ComponentBuild build;
    DrawList dynamic;
    DrawList chrome;
};

ScatterRun runScatter(const test::Fixture& fx, const PlotCase& c, float dpr) {
    const ScatterBody body = ScatterBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs in = plotInputs(fx, c, namesOf(body.channels()), dpr);
    plot::detail::ScatterFrame frame = plot::detail::scatterFrame(in, body);
    if (c.name == "scatter_tiled_colour") {
        frame.zBands = brakeBands();
    }
    const plot::detail::ScatterScene scene = scatterScene(in, body, frame, c.name);
    plot::detail::fitScatterScales(frame, scene);
    ScatterRun run;
    run.build.list = geom::newList(in);
    plot::detail::emitScatterStatic(run.build, in, body, frame, scene);
    run.dynamic = ScatterGeometry::buildDynamic(in);
    run.chrome = ScatterGeometry::buildChrome(in, false);
    return run;
}

// ---- histogram

std::vector<PlotCase> histogramCases() {
    std::vector<PlotCase> cases;
    {
        PlotCase c;
        c.name = "histogram_bars";
        c.type = "histogram";
        c.body = R"({"traces":[{"channel":"Corr Speed"}],"yMode":"percent","style":"bar",)"
                 R"("colourChannel":{"channel":"Throttle Pos","axis":null}})";
        c.legend = true;
        c.zoom = {0.0, 30.0e6};
        c.cursorT = 14 * kSec;
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    {
        PlotCase c;
        c.name = "histogram_lines_tiled";
        c.type = "histogram";
        c.body = R"({"traces":[{"channel":"Corr Speed"},{"channel":"Ground Speed"}],"yMode":"count",)"
                 R"("style":"line","layout":"tiled","tileCols":1,"stackLines":true,"condensedAxes":true,)"
                 R"("binLabelStep":2})";
        c.dark = true;
        c.height = 320;
        c.zoom = {0.0, 60.0e6};
        c.cursorT = 40 * kSec;
        c.activeChannel = "Ground Speed";
        c.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        cases.push_back(c);
    }
    {
        PlotCase c;
        c.name = "histogram_lines_stacked";
        c.type = "histogram";
        c.body = R"({"traces":[{"channel":"Corr Speed"},{"channel":"Ground Speed"}],"yMode":"percent",)"
                 R"("style":"line","stackLines":true,"binLabelStep":3})";
        c.zoom = {0.0, 60.0e6};
        c.cursorT = 20 * kSec;
        c.activeChannel = "Corr Speed";
        c.slots = {{'a', 2, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        PlotCase c;
        c.name = "histogram_text";
        c.type = "histogram";
        c.body = R"({"traces":[{"channel":"Corr Speed"}],"yMode":"time","style":"text"})";
        c.zoom = {0.0, 60.0e6};
        c.cursorT = 10 * kSec;
        c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay0, 0}};
        cases.push_back(c);
    }
    return cases;
}

// Speed bins in km/h: 25 km/h wide from 50 to 250, open below and above.
std::vector<analysis::Band> speedBins() {
    std::vector<analysis::Band> bins;
    bins.push_back(analysis::Band{-kInf, 50.0, 0, true, false});
    for (double lo = 50.0; lo < 250.0; lo += 25.0) {
        bins.push_back(analysis::Band{lo, lo + 25.0, 0, false, false});
    }
    bins.push_back(analysis::Band{250.0, kInf, 0, false, true});
    return bins;
}

std::vector<analysis::Band> throttleBands() {
    return {analysis::Band{0.0, 25.0, 0x90CAF9FFU, false, false},
            analysis::Band{25.0, 75.0, 0x42A5F5FFU, false, false},
            analysis::Band{75.0, 100.01, 0x0D47A1FFU, false, false}};
}

std::optional<std::size_t> bandOf(double v, const std::vector<analysis::Band>& bands) {
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const analysis::Band& b = bands[i];
        if (b.lessMin ? v < b.hi : (b.greaterMax ? v >= b.lo : (v >= b.lo && v < b.hi))) {
            return i;
        }
    }
    return std::nullopt;
}

plot::detail::HistogramScene histogramScene(const ComponentBuildInputs& in, const HistogramBody& body,
                                            const plot::detail::HistogramFrame& frame) {
    plot::detail::HistogramScene scene;
    for (std::size_t k = in.slots.size(); k-- > 0;) {
        const SlotData& slot = in.slots[k];
        const auto [i0, i1] = spanOf(in, slot);
        for (std::size_t n = 0; n < frame.traces.size(); ++n) {
            const int t = frame.traces[n];
            plot::detail::HistogramSet set;
            set.trace = t;
            set.slot = k;
            set.tile = frame.tiles.size() > 1 ? static_cast<int>(n) : 0;
            set.result.counts.assign(frame.bins.size(), 0.0);
            if (!frame.colourBands.empty()) {
                set.result.colourSplit.assign(frame.bins.size(),
                                              std::vector<double>(frame.colourBands.size(), 0.0));
            }
            for (SampleIdx i = i0; i < i1; ++i) {
                const double v = valueAt(slot, body.traces[static_cast<std::size_t>(t)].channel, i);
                const auto b = bandOf(v, frame.bins);
                if (!std::isfinite(v) || !b) {
                    continue;
                }
                set.result.counts[*b] += 1.0;
                ++set.result.accepted;
                if (!frame.colourBands.empty()) {
                    if (const auto z = bandOf(valueAt(slot, body.colourChannel, i), frame.colourBands)) {
                        set.result.colourSplit[*b][*z] += 1.0;
                    }
                }
            }
            set.result.seconds = static_cast<double>(set.result.accepted) * 0.05;
            const auto toMode = [&](double count) {
                switch (frame.yMode) {
                case analysis::HistogramYMode::Count:
                    return count;
                case analysis::HistogramYMode::Percent:
                    return set.result.accepted > 0 ? 100.0 * count / static_cast<double>(set.result.accepted)
                                                   : 0.0;
                case analysis::HistogramYMode::Time:
                    return count * 0.05;
                }
                return count;
            };
            for (const double c : set.result.counts) {
                set.result.values.push_back(toMode(c));
            }
            for (std::vector<double>& split : set.result.colourSplit) {
                for (double& c : split) {
                    c = toMode(c);
                }
            }
            scene.sets.push_back(std::move(set));
        }
    }
    return scene;
}

struct HistogramRun {
    ComponentBuild build;
    DrawList dynamic;
    DrawList chrome;
};

HistogramRun runHistogram(const test::Fixture& fx, const PlotCase& c, float dpr) {
    const HistogramBody body = HistogramBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs in = plotInputs(fx, c, namesOf(body.channels()), dpr);
    plot::detail::HistogramFrame frame = plot::detail::histogramFrame(in, body);
    frame.bins = speedBins();
    if (!body.colourChannel.empty()) {
        frame.colourBands = throttleBands();
    }
    const plot::detail::HistogramScene scene = histogramScene(in, body, frame);
    plot::detail::fitHistogramScales(frame, body, scene);
    HistogramRun run;
    run.build.list = geom::newList(in);
    plot::detail::emitHistogramStatic(run.build, in, body, frame, scene);
    // The highlighted bin: the active channel's Main value at the cursor.
    const SlotData& main = in.slots.front();
    const int active = c.activeChannel.empty() ? 0 : (c.activeChannel == "Ground Speed" ? 1 : 0);
    const double v = valueAt(main, body.traces[static_cast<std::size_t>(active)].channel,
                             (main.range.start + c.cursorT) / kPeriod);
    run.dynamic = geom::newList(in);
    plot::detail::emitHistogramDynamic(run.dynamic, in, body, frame, bandOf(v, frame.bins));
    run.chrome = HistogramGeometry::buildChrome(in, false);
    return run;
}

std::size_t countKind(const DrawList& list, CmdKind kind) {
    std::size_t n = 0;
    for (const DrawCmd& c : list.cmds) {
        n += c.kind == kind ? 1 : 0;
    }
    return n;
}

bool hasText(const DrawList& list, std::string_view needle) {
    for (const TextRun& r : list.texts) {
        if (list.textOf(r).find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- scatter

TEST_CASE("ScatterGeometry: frame, point sets with Main-lap tags, trend and equation, cursor ring",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const PlotCase c = scatterCases().front();
    const ScatterBody body = ScatterBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs in = plotInputs(fx, c, namesOf(body.channels()), 1.0F);
    plot::detail::ScatterFrame frame = plot::detail::scatterFrame(in, body);
    REQUIRE(frame.tiles.size() == 1);
    // The scales come from the whole-column summaries (Corr Speed in km/h, Throttle Pos in %).
    REQUIRE(frame.tiles[0].x.valid());
    REQUIRE(frame.tiles[0].y.valid());
    CHECK(frame.tiles[0].y.vMin >= 0.0);
    CHECK(frame.tiles[0].y.vMax <= 100.0);
    CHECK(frame.tiles[0].xTitleText == "Corr Speed [km/h]");
    CHECK(frame.tiles[0].yTitleText == "Throttle Pos [%]");
    const plot::detail::ScatterScene scene = scatterScene(in, body, frame, c.name);
    ComponentBuild build;
    build.list = geom::newList(in);
    plot::detail::emitScatterStatic(build, in, body, frame, scene);
    // A click on a Main point finds it and its Main-lap time (D-196).
    const plot::detail::ScatterSet& mainSet = scene.sets.back();
    REQUIRE(mainSet.slot == 0);
    const float px = plot::detail::xPxOf(frame.tiles[0], mainSet.series.x[7]);
    const float py = plot::detail::yPxOf(frame.tiles[0], mainSet.series.y[7]);
    std::int64_t tag = 0;
    const HitResult hit = build.hits.nearestPoint(px, py, 0.5F, &tag);
    REQUIRE(hit.kind == HitKind::Point);
    CHECK(hit.slot == 0);
    CHECK(tag == mainSet.series.t[static_cast<std::size_t>(hit.point)] - in.slots[0].range.start);
    CHECK(mainSet.series.x[static_cast<std::size_t>(hit.point)] == mainSet.series.x[7]);
    // Overlay points are drawn but not clickable: two Points commands, one point set.
    CHECK(countKind(build.list, CmdKind::Points) == 2);
    CHECK(hasText(build.list, "y = "));
    // Dynamic: a ring at the Main cursor point and a hollow datum square.
    const DrawList dyn = ScatterGeometry::buildDynamic(in);
    CHECK(countKind(dyn, CmdKind::Polyline) == 2); // the ring and the datum square
    CHECK(hasText(ScatterGeometry::buildChrome(in, false), "Throttle Pos vs Corr Speed"));
}

TEST_CASE("ScatterGeometry: buildStatic gathers through ScatterBinner", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const PlotCase c = scatterCases().front();
    const ScatterBody body = ScatterBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs in = plotInputs(fx, c, namesOf(body.channels()), 1.0F);
    const Result<ComponentBuild> build = ScatterGeometry::buildStatic(in, CancelToken{});
    if (test::scatterBinnerIsStub()) {
        REQUIRE_FALSE(build.ok());
        CHECK(build.error().code == ErrorCode::Unsupported);
        WARN("analysis::ScatterBinner is an M3 stub: the end-to-end scatter build is not checked");
        return;
    }
    REQUIRE(build.ok());
    // As many clickable Main points as the reference gather finds.
    const plot::detail::ScatterFrame frame = plot::detail::scatterFrame(in, body);
    const plot::detail::ScatterScene scene = scatterScene(in, body, frame, "");
    std::size_t drawn = 0;
    for (const DrawCmd& cmd : build->list.cmds) {
        drawn += cmd.kind == CmdKind::Points ? cmd.vertexCount : 0;
    }
    std::size_t expected = 0;
    for (const plot::detail::ScatterSet& s : scene.sets) {
        expected += s.series.x.size();
    }
    CHECK(drawn == expected);
}

TEST_CASE("ScatterGeometry: an overlay shown in another display unit draws in Main's unit", "[unit]") {
    // D-196 (f): the overlay's Corr Speed shown in mph converts into Main's km/h, so the plot is the one
    // drawn when both show km/h (an overlay in its own unit would sit at 0.62 x its speed on a km/h axis).
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const PlotCase c = scatterCases().front(); // Main 'a' and an overlay 'b'
    const ScatterBody body = ScatterBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs same = plotInputs(fx, c, namesOf(body.channels()), 1.0F);
    ComponentBuildInputs mixed = same;
    REQUIRE(mixed.slots.size() == 2);
    bool changed = false;
    for (ResolvedChannel& rc : mixed.slots[1].channels) {
        if (rc.name == "Corr Speed") {
            rc.toDisplay = units::Converter{3.6 / 1.609344, 0.0};
            rc.displayUnit = "mph";
            changed = true;
        }
    }
    REQUIRE(changed);
    const Result<ComponentBuild> a = ScatterGeometry::buildStatic(same, CancelToken{});
    const Result<ComponentBuild> b = ScatterGeometry::buildStatic(mixed, CancelToken{});
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a->list.verts.size() == b->list.verts.size());
    for (std::size_t i = 0; i < a->list.verts.size(); ++i) {
        INFO("vertex coordinate " << i);
        CHECK(b->list.verts[i] == Catch::Approx(a->list.verts[i]).margin(1e-3));
    }
}

TEST_CASE("ScatterGeometry: tiled layout, colour bands and the legend, density and interval average",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const std::vector<PlotCase> cases = scatterCases();
    const ScatterRun tiled = runScatter(fx, cases[1], 1.0F);
    // Two tiles (two Panel hit regions), a Points command per band colour used, the legend's swatches.
    std::size_t colours = 0;
    std::size_t swatches = 0;
    for (const DrawCmd& cmd : tiled.build.list.cmds) {
        colours += cmd.kind == CmdKind::Points ? 1 : 0;
        swatches += cmd.kind == CmdKind::FilledRect && cmd.rect.w == 10.0F ? 1 : 0;
    }
    CHECK(colours >= 3);
    CHECK(swatches == 4);
    CHECK(tiled.build.hits.hit(100.0F, 100.0F).kind == HitKind::Panel);
    CHECK(tiled.build.hits.hit(100.0F, 100.0F).group == 0);
    CHECK(tiled.build.hits.hit(330.0F, 100.0F).group == 1);
    CHECK(countKind(tiled.dynamic, CmdKind::Lines) == 2); // the cross cursor in each tile
    const ScatterRun density = runScatter(fx, cases[2], 1.0F);
    CHECK(countKind(density.build.list, CmdKind::Quads) > 0);
    CHECK(countKind(density.build.list, CmdKind::Points) == 0);
    CHECK(countKind(density.build.list, CmdKind::Polyline) == 1 + 2); // the plot frame, the interval averages
    CHECK(density.dynamic.cmds.empty());                              // cursorStyle none
}

TEST_CASE("ScatterGeometry: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    for (const PlotCase& c : scatterCases()) {
        for (const float dpr : {1.0F, 2.0F}) {
            const ScatterRun run = runScatter(fx, c, dpr);
            test::checkGolden("scatter", c.name, dpr,
                              test::layersJson(c.name, dpr, run.build.list, run.dynamic, run.chrome));
        }
    }
}

// ---- histogram

TEST_CASE("HistogramGeometry: bins as slots, overlays outlined, colour sub-bars, bin hits and the cursor bin",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const std::vector<PlotCase> cases = histogramCases();
    const HistogramRun bars = runHistogram(fx, cases[0], 1.0F);
    // Bin hit regions: ten bins over the plot.
    std::vector<int> bins;
    for (float x = 0.0F; x < 420.0F; x += 2.0F) {
        const HitResult h = bars.build.hits.hit(x, 120.0F);
        if (h.kind == HitKind::Bin && (bins.empty() || bins.back() != h.bin)) {
            bins.push_back(h.bin);
        }
    }
    REQUIRE(bins.size() == 10);
    CHECK(bins.front() == 0);
    CHECK(bins.back() == 9);
    CHECK(countKind(bars.build.list, CmdKind::Polyline) > 0); // outlined overlay bars (rectOutline)
    CHECK(hasText(bars.build.list, "Percent [%]"));
    CHECK(hasText(bars.build.list, "Corr Speed [km/h]"));
    CHECK(countKind(bars.dynamic, CmdKind::FilledRect) == 1);
    const HistogramRun lines = runHistogram(fx, cases[1], 1.0F);
    CHECK(countKind(lines.build.list, CmdKind::Polyline) == 2 + 4); // two tile frames, two traces x two slots
    // Stacked lines in one tile: the second trace lifted by 12 px against the first.
    const HistogramRun stacked = runHistogram(fx, cases[2], 1.0F);
    std::vector<float> firstY;
    for (const DrawCmd& cmd : stacked.build.list.cmds) {
        if (cmd.kind == CmdKind::Polyline && cmd.widthPx == 1.5F) {
            firstY.push_back(stacked.build.list.verts[static_cast<std::size_t>(cmd.vertexOffset) * 2 + 1]);
        }
    }
    REQUIRE(firstY.size() == 2);
    CHECK(firstY[0] - firstY[1] == 12.0F); // both start in the empty lessMin bin, on the baseline
    const HistogramRun text = runHistogram(fx, cases[3], 1.0F);
    CHECK(hasText(text.build.list, "Bin"));
    CHECK(hasText(text.build.list, "Corr Speed (Lap 1)"));
    CHECK(text.dynamic.cmds.empty()); // no highlight in text style
}

TEST_CASE("HistogramGeometry: buildStatic bins through generateBands and HistogramBinner", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    if (test::bandAxisIsStub() || test::histogramBinnerIsStub()) {
        WARN("analysis::BandAxis / HistogramBinner are M3 stubs: the end-to-end histogram build is not "
             "checked");
        return;
    }
    PlotCase c = histogramCases()[0];
    c.body =
        R"({"traces":[{"channel":"Corr Speed"}],"yMode":"count","bins":{"mode":"even","bandMode":"width",)"
        R"("dataMin":{"v":50,"unit":"km/h"},"dataMax":{"v":250,"unit":"km/h"},"bandWidth":{"v":25,"unit":"km/h"},)"
        R"("lessMinBand":false,"greaterMaxBand":false}})";
    c.slots = {{'a', 0, SlotKind::Main, 0}};
    const HistogramBody body = HistogramBody::parse(JsonDoc::parse(c.body).value()).value();
    const ComponentBuildInputs in = plotInputs(fx, c, namesOf(body.channels()), 1.0F);
    const Result<ComponentBuild> build = HistogramGeometry::buildStatic(in, CancelToken{});
    REQUIRE(build.ok());
    const plot::detail::HistogramFrame frame = plot::detail::histogramFrame(in, body);
    CHECK(frame.bins.size() == 8);
    std::size_t bars = 0;
    for (const DrawCmd& cmd : build->list.cmds) {
        bars += cmd.kind == CmdKind::FilledRect && cmd.rect.h > 0.0F ? 1 : 0;
    }
    CHECK(bars > 0);
}

TEST_CASE("HistogramGeometry: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    for (const PlotCase& c : histogramCases()) {
        for (const float dpr : {1.0F, 2.0F}) {
            const HistogramRun run = runHistogram(fx, c, dpr);
            test::checkGolden("histogram", c.name, dpr,
                              test::layersJson(c.name, dpr, run.build.list, run.dynamic, run.chrome));
        }
    }
}
