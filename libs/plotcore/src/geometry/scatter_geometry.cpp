// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScatterBody and ScatterGeometry (04 section 10; plot_scenes.hpp has the frame / scene / emit split).
// Layout (logical px x dpr): caption 18; the colour legend on the right when K (spec.legend.colour) is on and
// a colour channel is set (swatches of the colour bands with their lower limits); the rest holds one tile
// (overlapped, average) or one tile per visible trace (tiled: tileCols, else ceil(sqrt(N)) columns). Scales:
// manual limits converted to the axis display unit; channel and auto use the Main source's whole-column
// SummaryPyramid (x: the X channel; y: the union over the tile's Y channels; z: the colour channel), so the
// Dynamic cursor ring lands on the Static axes; while a summary is incomplete the Static build takes the data
// extents and the Dynamic build draws no ring. Points per set in its key colour (overlays first, Main last),
// coloured by colour band when the colour channel is set; above the density threshold (density.auto) every
// set is drawn as a 2D histogram (16 levels of its colour as Quads). Trend lines (64 segments over the x
// scale) and the interval average (a stepped line) per set; the trend equation of the Main sets at the top
// left of their tile. HitTester point sets: the Main sets only (04 section 10.1 item 7), tagged with each
// point's Main-lap time (D-196).

#include "body_json.hpp"
#include "geometry_common.hpp"
#include "plot_scenes.hpp"

#include <esses/store/resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>

namespace esses::plot {

namespace {

using nlohmann::json;

const char* trendTypeText(analysis::TrendType t) noexcept {
    switch (t) {
    case analysis::TrendType::None:
        return "none";
    case analysis::TrendType::Linear:
        return "linear";
    case analysis::TrendType::Polynomial:
        return "poly";
    }
    return "linear";
}

std::string enumOf(const json& j, const std::string& pointer, std::initializer_list<const char*> allowed) {
    const std::string& text = jsonio::asString(j, pointer);
    for (const char* a : allowed) {
        if (text == a) {
            return text;
        }
    }
    jsonio::violate(pointer, "unexpected value \"" + text + "\"");
}

analysis::BandAxis bandAxisOf(const json& j, const std::string& pointer) {
    Result<JsonDoc> doc = jsonio::toDoc(j);
    if (!doc) {
        jsonio::violate(pointer, doc.error().message);
    }
    Result<analysis::BandAxis> axis = analysis::BandAxis::fromJson(*doc);
    if (!axis) {
        const std::string inner = axis.error().context.detail.value_or(std::string{});
        jsonio::violate(pointer + inner, axis.error().message, axis.error().code);
    }
    return std::move(*axis);
}

} // namespace

// ---- ScatterBody

Result<ScatterBody> ScatterBody::parse(const JsonDoc& body) {
    return jsonio::guardJson([&]() -> Result<ScatterBody> {
        ScatterBody out;
        out.rest = body;
        if (body.isNull()) {
            return out;
        }
        const json j = jsonio::fromDoc(body);
        if (!j.is_object()) {
            jsonio::violate("", "scatter body is not an object");
        }
        jsonio::ObjectReader r(j, "");
        if (const json* v = r.optional("xChannel")) {
            out.xChannel = jsonio::asString(*v, r.child("xChannel"));
        }
        if (const json* tracesJ = r.optional("traces")) {
            if (!tracesJ->is_array()) {
                jsonio::violate(r.child("traces"), "expected an array");
            }
            for (std::size_t i = 0; i < tracesJ->size(); ++i) {
                const std::string ptr = jsonio::pointerIndex(r.child("traces"), i);
                const json& tj = (*tracesJ)[i];
                if (!tj.is_object()) {
                    jsonio::violate(ptr, "expected an object");
                }
                jsonio::ObjectReader t(tj, ptr);
                ScatterTrace trace;
                trace.yChannel = jsonio::asString(t.required("yChannel"), t.child("yChannel"));
                if (const json* v = t.optional("visible")) {
                    trace.visible = jsonio::asBool(*v, t.child("visible"));
                }
                if (const json* v = t.optional("yDelayUs")) {
                    trace.yDelayUs = jsonio::asInt(*v, t.child("yDelayUs"));
                }
                out.traces.push_back(std::move(trace));
            }
        }
        if (const json* colour = r.optional("colourChannel")) {
            if (!colour->is_object()) {
                jsonio::violate(r.child("colourChannel"), "expected an object");
            }
            jsonio::ObjectReader c(*colour, r.child("colourChannel"));
            if (const json* v = c.optional("channel")) {
                out.colourChannel = jsonio::asString(*v, c.child("channel"));
            }
            if (const json* v = c.optional("axis")) {
                out.colourAxis = bandAxisOf(*v, c.child("axis"));
            }
        }
        if (const json* gating = r.optional("gating")) {
            if (!gating->is_object()) {
                jsonio::violate(r.child("gating"), "expected an object");
            }
            jsonio::ObjectReader g(*gating, r.child("gating"));
            if (const json* v = g.optional("channel")) {
                out.gatingChannel = jsonio::asString(*v, g.child("channel"));
            }
        }
        if (const json* v = r.optional("layout")) {
            out.layout = enumOf(*v, r.child("layout"), {"overlapped", "tiled", "average"});
        }
        if (const json* v = r.optional("tileCols")) {
            out.tileCols = jsonio::asInt32(*v, r.child("tileCols"));
        }
        if (const json* v = r.optional("style")) {
            out.style = enumOf(*v, r.child("style"), {"points", "pointsTrend", "trend"});
        }
        if (const json* v = r.optional("pointSize")) {
            out.pointSize = static_cast<float>(jsonio::asNumber(*v, r.child("pointSize")));
        }
        if (const json* trendJ = r.optional("trend")) {
            if (!trendJ->is_object()) {
                jsonio::violate(r.child("trend"), "expected an object");
            }
            jsonio::ObjectReader t(*trendJ, r.child("trend"));
            if (const json* v = t.optional("type")) {
                const std::string type = enumOf(*v, t.child("type"), {"none", "linear", "poly"});
                out.trend.type = type == "none"     ? analysis::TrendType::None
                                 : type == "linear" ? analysis::TrendType::Linear
                                                    : analysis::TrendType::Polynomial;
            }
            if (const json* v = t.optional("order")) {
                out.trend.order = jsonio::asInt32(*v, t.child("order"));
            }
            if (const json* v = t.optional("throughZero")) {
                out.trend.throughZero = jsonio::asBool(*v, t.child("throughZero"));
            }
            if (const json* v = t.optional("showEquation")) {
                out.showEquation = jsonio::asBool(*v, t.child("showEquation"));
            }
            if (const json* v = t.optional("significantDigits")) {
                out.significantDigits = jsonio::asInt32(*v, t.child("significantDigits"));
            }
        }
        if (const json* interval = r.optional("intervalAverage")) {
            if (!interval->is_object()) {
                jsonio::violate(r.child("intervalAverage"), "expected an object");
            }
            jsonio::ObjectReader t(*interval, r.child("intervalAverage"));
            if (const json* v = t.optional("enabled")) {
                out.intervalAverage = jsonio::asBool(*v, t.child("enabled"));
            }
            if (const json* v = t.optional("binWidth")) {
                out.intervalBinWidth = detail::readUnitValue(*v, t.child("binWidth"));
            }
        }
        if (const json* scales = r.optional("scales")) {
            if (!scales->is_object()) {
                jsonio::violate(r.child("scales"), "expected an object");
            }
            jsonio::ObjectReader s(*scales, r.child("scales"));
            if (const json* v = s.optional("x")) {
                out.xScale = detail::readScaleSpec(*v, s.child("x"));
            }
            if (const json* v = s.optional("y")) {
                out.yScale = detail::readScaleSpec(*v, s.child("y"));
            }
            if (const json* v = s.optional("z")) {
                out.zScale = detail::readScaleSpec(*v, s.child("z"));
            }
        }
        if (const json* v = r.optional("cursorStyle")) {
            out.cursorStyle = enumOf(*v, r.child("cursorStyle"), {"cross", "ring", "none"});
        }
        if (const json* density = r.optional("density")) {
            if (!density->is_object()) {
                jsonio::violate(r.child("density"), "expected an object");
            }
            jsonio::ObjectReader d(*density, r.child("density"));
            if (const json* v = d.optional("auto")) {
                out.densityAuto = jsonio::asBool(*v, d.child("auto"));
            }
            if (const json* v = d.optional("threshold")) {
                out.densityThreshold = jsonio::asU64(*v, d.child("threshold"));
            }
            if (const json* v = d.optional("bins")) {
                if (!v->is_array() || v->size() != 2) {
                    jsonio::violate(d.child("bins"), "expected [x, y]");
                }
                out.densityBinsX = jsonio::asInt32((*v)[0], jsonio::pointerIndex(d.child("bins"), 0));
                out.densityBinsY = jsonio::asInt32((*v)[1], jsonio::pointerIndex(d.child("bins"), 1));
            }
        }
        if (const json* v = r.optional("showGrid")) {
            out.showGrid = jsonio::asBool(*v, r.child("showGrid"));
        }
        return out;
    });
}

JsonDoc ScatterBody::toJson() const {
    const ScatterBody d;
    json j = detail::mergeOver(rest);
    detail::put(j, "xChannel", xChannel, d.xChannel);
    if (!traces.empty() || j.contains("traces")) {
        const json old = j.contains("traces") && j["traces"].is_array() ? j["traces"] : json::array();
        json arr = json::array();
        for (std::size_t i = 0; i < traces.size(); ++i) {
            const ScatterTrace& t = traces[i];
            const ScatterTrace td;
            json tj = i < old.size() && old[i].is_object() ? old[i] : json::object();
            tj["yChannel"] = t.yChannel;
            detail::put(tj, "visible", t.visible, td.visible);
            detail::put(tj, "yDelayUs", t.yDelayUs, td.yDelayUs);
            arr.push_back(std::move(tj));
        }
        j["traces"] = std::move(arr);
    }
    json colour = detail::childObject(j, "colourChannel");
    detail::put(colour, "channel", colourChannel, d.colourChannel);
    if (colourAxis) {
        const json old = colour.contains("axis") ? colour["axis"] : json();
        colour["axis"] = detail::bandAxisJson(old.is_object() ? &old : nullptr, *colourAxis);
    } else {
        colour.erase("axis");
    }
    detail::putObject(j, "colourChannel", std::move(colour));
    json gating = detail::childObject(j, "gating");
    detail::put(gating, "channel", gatingChannel, d.gatingChannel);
    detail::putObject(j, "gating", std::move(gating));
    detail::put(j, "layout", layout, d.layout);
    detail::put(j, "tileCols", tileCols, d.tileCols);
    detail::put(j, "style", style, d.style);
    detail::put(j, "pointSize", static_cast<double>(pointSize), static_cast<double>(d.pointSize));
    json trendJ = detail::childObject(j, "trend");
    detail::put(trendJ, "type", std::string(trendTypeText(trend.type)),
                std::string(trendTypeText(d.trend.type)));
    detail::put(trendJ, "order", trend.order, d.trend.order);
    detail::put(trendJ, "throughZero", trend.throughZero, d.trend.throughZero);
    detail::put(trendJ, "showEquation", showEquation, d.showEquation);
    detail::put(trendJ, "significantDigits", significantDigits, d.significantDigits);
    detail::putObject(j, "trend", std::move(trendJ));
    json interval = detail::childObject(j, "intervalAverage");
    detail::put(interval, "enabled", intervalAverage, d.intervalAverage);
    if (intervalBinWidth) {
        interval["binWidth"] = detail::unitValueJson(*intervalBinWidth);
    } else {
        interval.erase("binWidth");
    }
    detail::putObject(j, "intervalAverage", std::move(interval));
    json scales = detail::childObject(j, "scales");
    const auto putScale = [&scales](const char* key, const ScaleSpec& s) {
        if (scales.contains(key) || !(s == ScaleSpec{})) {
            scales[key] = detail::scaleSpecJson(s);
        }
    };
    putScale("x", xScale);
    putScale("y", yScale);
    putScale("z", zScale);
    detail::putObject(j, "scales", std::move(scales));
    detail::put(j, "cursorStyle", cursorStyle, d.cursorStyle);
    json density = detail::childObject(j, "density");
    detail::put(density, "auto", densityAuto, d.densityAuto);
    detail::put(density, "threshold", densityThreshold, d.densityThreshold);
    if (density.contains("bins") || densityBinsX != d.densityBinsX || densityBinsY != d.densityBinsY) {
        density["bins"] = json::array({densityBinsX, densityBinsY});
    }
    detail::putObject(j, "density", std::move(density));
    detail::put(j, "showGrid", showGrid, d.showGrid);
    return detail::docOf(j, rest);
}

std::vector<ChannelUse> ScatterBody::channels() const {
    std::vector<ChannelUse> out;
    out.reserve(traces.size() + 3);
    out.push_back(ChannelUse{xChannel, {}, std::nullopt});
    for (const ScatterTrace& t : traces) {
        out.push_back(ChannelUse{t.yChannel, {}, std::nullopt});
    }
    if (!colourChannel.empty()) {
        out.push_back(ChannelUse{colourChannel, {}, std::nullopt});
    }
    if (!gatingChannel.empty()) {
        out.push_back(ChannelUse{gatingChannel, {}, std::nullopt});
    }
    return out;
}

// ---- frame, scene and emission

namespace detail {

namespace {

constexpr int kTrendSegments = 64;
constexpr int kDensityLevels = 16;

std::size_t xIndex() {
    return 0;
}
std::size_t yIndex(int trace) {
    return 1 + static_cast<std::size_t>(trace);
}
std::size_t zIndex(const ScatterBody& body) {
    return 1 + body.traces.size();
}
std::size_t gIndex(const ScatterBody& body) {
    return 1 + body.traces.size() + (body.colourChannel.empty() ? 0 : 1);
}

std::string titleOf(const std::string& channel, const ResolvedChannel* rc) {
    if (rc == nullptr || !rc->descriptor || rc->displayUnit.empty()) {
        return channel;
    }
    return channel + " [" + rc->displayUnit + "]";
}

// The channel's own interpolation (D-194 (a)); Linear without a descriptor.
store::Interp interpOf(const ResolvedChannel& rc) {
    const std::optional<model::ChannelDescriptor>& d = rc.descriptor;
    return d.has_value() ? d->interp : store::Interp::Linear;
}

int colourIndexOf(const ResolvedChannel* rc, int fallback) {
    if (rc != nullptr && rc->descriptor && rc->descriptor->colourIndex >= 0) {
        return rc->descriptor->colourIndex;
    }
    return fallback;
}

// A scale from a ScaleSpec: manual limits in the axis unit, else the SummaryPyramid of the channels (an
// incomplete summary leaves the scale invalid).
geom::Scale scaleOf(const ScaleSpec& spec, const std::vector<const ResolvedChannel*>& channels,
                    const units::UnitRegistry* registry) {
    geom::Scale s;
    const ResolvedChannel* axis = nullptr;
    for (const ResolvedChannel* rc : channels) {
        if (geom::readable(rc)) {
            axis = rc;
            break;
        }
    }
    if (spec.mode == ScaleMode::Manual && spec.min && spec.max) {
        const std::string unit = axis != nullptr ? axis->displayUnit : spec.min->unit;
        const auto lo = geom::unitValueIn(*spec.min, unit, axis, registry);
        const auto hi = geom::unitValueIn(*spec.max, unit, axis, registry);
        if (lo && hi) {
            s.unite(*lo, *hi);
            s.widenDegenerate();
            return s;
        }
    }
    for (const ResolvedChannel* rc : channels) {
        if (!geom::readable(rc)) {
            continue;
        }
        const geom::SummaryRange sr = geom::summaryRange(*rc, rc->toDisplay);
        if (!sr.covered) {
            return geom::Scale{};
        }
        if (sr.range.valid()) {
            s.unite(sr.range.vMin, sr.range.vMax);
        }
    }
    s.widenDegenerate();
    return s;
}

// Main in its channel colours, the overlays in their range key colours (04 section 10.1 item 9, 06
// section 5.3 item 3).
Rgba setColour(const ComponentBuildInputs& in, const ScatterBody& body, const ScatterSet& set) {
    const SlotData& slot = in.slots[set.slot];
    if (slot.slot.kind != SlotKind::Main) {
        return rangeKeyColour(geom::schemeOf(in), slot.slot);
    }
    const ResolvedChannel* rc =
        set.trace >= 0 ? geom::channelOf(slot, yIndex(set.trace),
                                         body.traces[static_cast<std::size_t>(set.trace)].yChannel)
                       : nullptr;
    return traceColour(geom::schemeOf(in), in.selection, slot.slot,
                       colourIndexOf(rc, set.trace >= 0 ? set.trace : 0));
}

// The sets of one slot: the visible traces, or their mean (layout average), from ScatterBinner::gather.
Result<std::vector<ScatterSet>> gatherSlot(const ComponentBuildInputs& in, const ScatterBody& body,
                                           const ScatterFrame& frame, std::size_t k,
                                           const CancelToken& cancel) {
    std::vector<ScatterSet> out;
    const SlotData& slot = in.slots[k];
    const ResolvedChannel* x = geom::channelOf(slot, xIndex(), body.xChannel);
    if (!geom::readable(x)) {
        return out;
    }
    std::vector<analysis::SampleSpan> spans = geom::visibleSpans(in, slot);
    if (spans.empty()) {
        return out;
    }
    // Every slot draws in the Main slot's display units, the units of the axes and the trend equation: an
    // overlay whose channel shows another unit converts into Main's (D-196 (f)). A channel of the same
    // quantity has the same base unit (D-010), so Main's converter applies to it as it is.
    const SlotData& mainSlot = in.slots.front();
    const auto inMainUnit = [&](const ResolvedChannel& rc, const ResolvedChannel* mainRc) {
        if (k == 0 || mainRc == nullptr || !mainRc->descriptor || !rc.descriptor ||
            rc.displayUnit == mainRc->displayUnit ||
            rc.descriptor->quantity != mainRc->descriptor->quantity) {
            return rc.toDisplay;
        }
        return mainRc->toDisplay;
    };
    analysis::ScatterInput input;
    input.x = analysis::ScatterChannel{
        *x->series, inMainUnit(*x, geom::channelOf(mainSlot, xIndex(), body.xChannel)), interpOf(*x)};
    std::vector<int> traceOf;
    for (const int t : frame.traces) {
        const ScatterTrace& trace = body.traces[static_cast<std::size_t>(t)];
        const ResolvedChannel* y = geom::channelOf(slot, yIndex(t), trace.yChannel);
        if (!geom::readable(y)) {
            continue;
        }
        input.y.push_back(analysis::ScatterChannel{
            *y->series, inMainUnit(*y, geom::channelOf(mainSlot, yIndex(t), trace.yChannel)), interpOf(*y)});
        input.yDelayUs.push_back(trace.yDelayUs);
        traceOf.push_back(t);
    }
    if (input.y.empty()) {
        return out;
    }
    if (!body.colourChannel.empty()) {
        const ResolvedChannel* z = geom::channelOf(slot, zIndex(body), body.colourChannel);
        if (geom::readable(z)) {
            input.colour = analysis::ScatterChannel{
                *z->series, inMainUnit(*z, geom::channelOf(mainSlot, zIndex(body), body.colourChannel)),
                interpOf(*z)};
        }
    }
    if (!body.gatingChannel.empty()) {
        const ResolvedChannel* g = geom::channelOf(slot, gIndex(body), body.gatingChannel);
        if (geom::readable(g)) {
            input.gating = *g->series;
        }
    }
    input.spans = std::move(spans);
    Result<std::vector<analysis::ScatterSeries>> gathered =
        analysis::ScatterBinner::gather(input, Priority::Visible, cancel);
    if (!gathered) {
        if (gathered.error().code == ErrorCode::Cancelled ||
            gathered.error().code == ErrorCode::Unsupported) {
            return Failure{gathered.error()};
        }
        return out; // no points from this slot (e.g. an X channel without a rate)
    }
    if (frame.average) {
        struct Acc {
            double x = 0.0;
            double z = 0.0;
            double sum = 0.0;
            int n = 0;
        };
        std::map<Micros, Acc> byTime;
        for (const analysis::ScatterSeries& s : *gathered) {
            for (std::size_t i = 0; i < s.t.size() && i < s.x.size() && i < s.y.size(); ++i) {
                Acc& a = byTime[s.t[i]];
                if (a.n == 0) {
                    a.x = s.x[i];
                    a.z = i < s.z.size() ? s.z[i] : 0.0;
                }
                a.sum += s.y[i];
                ++a.n;
            }
        }
        ScatterSet set;
        set.trace = -1;
        set.slot = k;
        const bool withZ = input.colour.has_value();
        for (const auto& [t, a] : byTime) {
            set.series.t.push_back(t);
            set.series.x.push_back(a.x);
            set.series.y.push_back(a.sum / static_cast<double>(a.n));
            if (withZ) {
                set.series.z.push_back(a.z);
            }
        }
        out.push_back(std::move(set));
        return out;
    }
    for (std::size_t i = 0; i < gathered->size() && i < traceOf.size(); ++i) {
        ScatterSet set;
        set.trace = traceOf[i];
        set.slot = k;
        if (body.layout == "tiled") {
            const auto it = std::find(frame.traces.begin(), frame.traces.end(), traceOf[i]);
            set.tile = static_cast<int>(it - frame.traces.begin());
        }
        set.series = std::move((*gathered)[i]);
        out.push_back(std::move(set));
    }
    return out;
}

// One point per vertex of a ring of radius r (an octagon; exact constants so every platform agrees).
std::vector<float> ringOf(float cx, float cy, float r) {
    constexpr float kS = 0.70710678F;
    const std::array<std::array<float, 2>, 9> unit{{{1.0F, 0.0F},
                                                    {kS, kS},
                                                    {0.0F, 1.0F},
                                                    {-kS, kS},
                                                    {-1.0F, 0.0F},
                                                    {-kS, -kS},
                                                    {0.0F, -1.0F},
                                                    {kS, -kS},
                                                    {1.0F, 0.0F}}};
    std::vector<float> xy;
    xy.reserve(unit.size() * 2);
    for (const auto& u : unit) {
        xy.push_back(cx + r * u[0]);
        xy.push_back(cy + r * u[1]);
    }
    return xy;
}

bool inside(const RectPx& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

} // namespace

ScatterFrame scatterFrame(const ComponentBuildInputs& in, const ScatterBody& body) {
    ScatterFrame f;
    const float dpr = geom::dprOf(in);
    const ITextMetrics& metrics = geom::metricsOf(in);
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    f.caption = geom::captionRect(in);
    for (std::size_t i = 0; i < body.traces.size(); ++i) {
        if (body.traces[i].visible && !body.traces[i].yChannel.empty()) {
            f.traces.push_back(static_cast<int>(i));
        }
    }
    f.average = body.layout == "average";
    const SlotData* main = in.slots.empty() ? nullptr : &in.slots.front();
    const ResolvedChannel* xRc = main != nullptr ? geom::channelOf(*main, xIndex(), body.xChannel) : nullptr;
    const ResolvedChannel* zRc = main != nullptr && !body.colourChannel.empty()
                                     ? geom::channelOf(*main, zIndex(body), body.colourChannel)
                                     : nullptr;
    // Colour bands and the legend.
    if (body.colourAxis && geom::readable(zRc)) {
        f.zUnit = zRc->displayUnit;
        const geom::Scale z = scaleOf(body.zScale, {zRc}, in.units.get());
        if (z.valid()) {
            Result<std::vector<analysis::Band>> bands =
                analysis::generateBands(*body.colourAxis, z.vMin, z.vMax, in.units.get(), f.zUnit);
            if (bands) {
                f.zBands = std::move(*bands);
            }
        }
    }
    float legendW = 0.0F;
    if (in.spec.legend.colour && !body.colourChannel.empty()) {
        const float pad = 3.0F * dpr;
        legendW = geom::snap(3.0F * pad + kLegendSwatchLogical * dpr +
                             metrics.advancePx(TextStyleId::ColourBand, "-88888.8", dpr));
        legendW = std::min(legendW, 0.4F * w);
    }
    const float top = f.caption.h;
    f.legend = legendW > 0.0F ? RectPx{w - legendW, top, legendW, std::max(0.0F, h - top)} : RectPx{};
    const RectPx area{0.0F, top, std::max(0.0F, w - legendW), std::max(0.0F, h - top)};
    const bool tiled = body.layout == "tiled" && f.traces.size() > 1;
    const int count = tiled ? static_cast<int>(f.traces.size()) : 1;
    f.tiles = layoutPlotTiles(area, count, tiled ? body.tileCols : 1, metrics, dpr);
    const std::string xTitle = titleOf(body.xChannel, xRc);
    for (std::size_t t = 0; t < f.tiles.size(); ++t) {
        PlotTile& tile = f.tiles[t];
        tile.trace = tiled ? f.traces[t] : -1;
        tile.xTitleText = xTitle;
        tile.x = scaleOf(body.xScale, {xRc}, in.units.get());
        std::vector<const ResolvedChannel*> ys;
        std::string yTitle;
        for (const int tr : f.traces) {
            if (tiled && tr != tile.trace) {
                continue;
            }
            const std::string& name = body.traces[static_cast<std::size_t>(tr)].yChannel;
            const ResolvedChannel* rc = main != nullptr ? geom::channelOf(*main, yIndex(tr), name) : nullptr;
            ys.push_back(rc);
            yTitle += (yTitle.empty() ? "" : ", ") + titleOf(name, rc);
        }
        tile.yTitleText = f.average && !yTitle.empty() ? "Average of " + yTitle : yTitle;
        tile.y = scaleOf(body.yScale, ys, in.units.get());
    }
    return f;
}

void fitScatterScales(ScatterFrame& frame, const ScatterScene& scene) {
    for (std::size_t t = 0; t < frame.tiles.size(); ++t) {
        PlotTile& tile = frame.tiles[t];
        const bool needX = !tile.x.valid();
        const bool needY = !tile.y.valid();
        if (!needX && !needY) {
            continue;
        }
        for (const ScatterSet& set : scene.sets) {
            if (set.tile != static_cast<int>(t)) {
                continue;
            }
            for (std::size_t i = 0; i < set.series.x.size() && i < set.series.y.size(); ++i) {
                if (needX) {
                    tile.x.unite(set.series.x[i], set.series.x[i]);
                }
                if (needY) {
                    tile.y.unite(set.series.y[i], set.series.y[i]);
                }
            }
        }
        tile.x.widenDegenerate();
        tile.y.widenDegenerate();
    }
}

void emitScatterStatic(ComponentBuild& build, const ComponentBuildInputs& in, const ScatterBody& body,
                       const ScatterFrame& frame, const ScatterScene& scene) {
    DrawList& list = build.list;
    const float dpr = geom::dprOf(in);
    const geom::Colours colours(geom::schemeOf(in));
    const ITextMetrics& metrics = geom::metricsOf(in);
    const float lineW = geom::lineWidthOf(dpr);
    list.clear(colours.background);
    for (std::size_t t = 0; t < frame.tiles.size(); ++t) {
        emitTileAxes(list, build.hits, frame.tiles[t], static_cast<int>(t), body.showGrid, true, colours,
                     metrics, dpr);
    }
    const bool points = body.style != "trend";
    const bool trends = body.style != "points";
    const float pointPx = std::max(1.0F, body.pointSize * dpr);
    for (const ScatterSet& set : scene.sets) {
        if (set.tile < 0 || static_cast<std::size_t>(set.tile) >= frame.tiles.size()) {
            continue;
        }
        const PlotTile& tile = frame.tiles[static_cast<std::size_t>(set.tile)];
        if (!(tile.plot.w > 0.0F) || !(tile.plot.h > 0.0F) || !tile.x.valid() || !tile.y.valid()) {
            continue;
        }
        const Rgba colour = setColour(in, body, set);
        list.scissor(tile.plot);
        if (scene.density && set.density.nx > 0 && set.density.ny > 0) {
            const analysis::DensityGrid& g = set.density;
            std::uint32_t peak = 0;
            for (const std::uint32_t c : g.counts) {
                peak = std::max(peak, c);
            }
            std::array<std::vector<float>, kDensityLevels> levels;
            const double cw = (g.x1 - g.x0) / static_cast<double>(g.nx);
            const double ch = (g.y1 - g.y0) / static_cast<double>(g.ny);
            for (int r = 0; r < g.ny && peak > 0; ++r) {
                for (int c = 0; c < g.nx; ++c) {
                    const std::uint32_t n =
                        g.counts[static_cast<std::size_t>(r) * static_cast<std::size_t>(g.nx) +
                                 static_cast<std::size_t>(c)];
                    if (n == 0) {
                        continue;
                    }
                    const auto level = static_cast<std::size_t>(std::min<std::uint64_t>(
                        kDensityLevels - 1, (static_cast<std::uint64_t>(n) * kDensityLevels - 1) / peak));
                    const float x0 = xPxOf(tile, g.x0 + cw * c);
                    const float x1 = xPxOf(tile, g.x0 + cw * (c + 1));
                    const float y0 = yPxOf(tile, g.y0 + ch * r);
                    const float y1 = yPxOf(tile, g.y0 + ch * (r + 1));
                    levels[level].insert(levels[level].end(), {x0, y0, x1, y0, x1, y1, x0, y1});
                }
            }
            for (std::size_t l = 0; l < levels.size(); ++l) {
                if (!levels[l].empty()) {
                    const auto a = static_cast<std::uint8_t>(0x30 + (0xFF - 0x30) * l / (kDensityLevels - 1));
                    geom::quads(list, levels[l].data(), levels[l].size() / 2, geom::withAlpha(colour, a));
                }
            }
        } else if (points) {
            std::map<Rgba, std::vector<float>> byColour;
            std::vector<float>& plain = byColour[colour];
            const bool banded = !frame.zBands.empty() && set.series.z.size() == set.series.x.size();
            for (std::size_t i = 0; i < set.series.x.size() && i < set.series.y.size(); ++i) {
                const float px = xPxOf(tile, set.series.x[i]);
                const float py = yPxOf(tile, set.series.y[i]);
                if (!std::isfinite(px) || !std::isfinite(py)) {
                    continue;
                }
                std::vector<float>* target = &plain;
                if (banded) {
                    if (const auto b = geom::bandIndexOf(set.series.z[i], frame.zBands)) {
                        target = &byColour[static_cast<Rgba>(frame.zBands[*b].colour)];
                    }
                }
                target->insert(target->end(), {px, py});
            }
            // Plain points first, then the bands in band order (deterministic).
            if (!plain.empty()) {
                list.points(plain.data(), plain.size() / 2, colour, pointPx);
            }
            for (const analysis::Band& band : frame.zBands) {
                const auto it = byColour.find(static_cast<Rgba>(band.colour));
                if (it != byColour.end() && it->first != colour && !it->second.empty()) {
                    list.points(it->second.data(), it->second.size() / 2, it->first, pointPx);
                    it->second.clear();
                }
            }
        }
        if (set.interval && !set.interval->binLo.empty() && body.intervalBinWidth) {
            const auto& iv = *set.interval;
            // The bin width in x units: the spacing of the bins (the smallest positive gap), else 1 % of the
            // scale.
            double width = 0.0;
            for (std::size_t i = 1; i < iv.binLo.size(); ++i) {
                const double gap = iv.binLo[i] - iv.binLo[i - 1];
                if (gap > 0.0 && (width == 0.0 || gap < width)) {
                    width = gap;
                }
            }
            if (!(width > 0.0)) {
                width = 0.01 * (tile.x.vMax - tile.x.vMin);
            }
            std::vector<float> steps;
            for (std::size_t i = 0; i < iv.binLo.size() && i < iv.mean.size(); ++i) {
                const float y = yPxOf(tile, iv.mean[i]);
                steps.insert(steps.end(), {xPxOf(tile, iv.binLo[i]), y, xPxOf(tile, iv.binLo[i] + width), y});
            }
            if (steps.size() >= 4) {
                list.polyline(steps.data(), steps.size() / 2, colour, 2.0F * lineW);
            }
        }
        if (trends && set.trend && !set.trend->coefficients.empty()) {
            std::vector<float> curve;
            for (int s = 0; s <= kTrendSegments; ++s) {
                const double x = tile.x.vMin + (tile.x.vMax - tile.x.vMin) * s / kTrendSegments;
                const double y = geom::polyval(set.trend->coefficients, x);
                const float px = xPxOf(tile, x);
                const float py = yPxOf(tile, y);
                if (std::isfinite(px) && std::isfinite(py)) {
                    curve.insert(curve.end(), {px, std::clamp(py, tile.plot.y - 4.0F * tile.plot.h,
                                                              tile.plot.y + 5.0F * tile.plot.h)});
                }
            }
            if (curve.size() >= 4) {
                const Slot& slot = in.slots[set.slot].slot;
                list.polyline(curve.data(), curve.size() / 2, colour,
                              1.5F * lineW * traceWidthScale(geom::schemeOf(in), slot),
                              traceDash(geom::schemeOf(in), slot));
            }
        }
        list.scissor(RectPx{});
        // Main point sets for nearestPoint (D-196 tags: the Main-lap time of each point).
        if (set.slot == 0 && points) {
            std::vector<PointPx> pts;
            std::vector<std::int64_t> tags;
            pts.reserve(set.series.x.size());
            for (std::size_t i = 0; i < set.series.x.size() && i < set.series.y.size(); ++i) {
                pts.push_back(PointPx{xPxOf(tile, set.series.x[i]), yPxOf(tile, set.series.y[i])});
                tags.push_back(i < set.series.t.size() ? set.series.t[i] - in.slots.front().range.start : 0);
            }
            HitResult base;
            base.kind = HitKind::Point;
            base.group = set.tile;
            base.trace = set.trace;
            base.slot = 0;
            build.hits.addPoints(tile.plot, pts, base, tags);
        }
    }
    // Trend equations of the Main sets, top left of their tile.
    if (trends && body.showEquation) {
        std::vector<float> lineY(frame.tiles.size(), 0.0F);
        const float eqLine = metrics.lineHeightPx(TextStyleId::Equation, dpr);
        for (const ScatterSet& set : scene.sets) {
            if (set.slot != 0 || set.equation.empty() || set.tile < 0 ||
                static_cast<std::size_t>(set.tile) >= frame.tiles.size()) {
                continue;
            }
            const auto ti = static_cast<std::size_t>(set.tile);
            const PlotTile& tile = frame.tiles[ti];
            list.textRun(set.equation, tile.plot.x + 4.0F * dpr, tile.plot.y + 4.0F * dpr + lineY[ti],
                         TextStyleId::Equation, TextAnchor::TopLeft, setColour(in, body, set),
                         std::max(1.0F, tile.plot.w - 8.0F * dpr), 0, colours.background);
            lineY[ti] += eqLine;
        }
    }
    emitBandLegend(list, frame.legend, frame.zBands, colours, metrics, dpr);
}

std::vector<ScatterCursorPoint> scatterCursorPoints(const ComponentBuildInputs& in, const ScatterBody& body,
                                                    const ScatterFrame& frame) {
    std::vector<ScatterCursorPoint> out;
    if (in.slots.empty()) {
        return out;
    }
    const SlotData& main = in.slots.front();
    const ResolvedChannel* x = geom::channelOf(main, xIndex(), body.xChannel);
    if (!geom::readable(x)) {
        return out;
    }
    const auto valueAt = [](const ResolvedChannel& rc, Micros t) {
        const store::ValueSample v = store::Resampler::sampleAt(*rc.series, t, rc.descriptor->interp);
        return v.state == store::SampleState::Valid ? rc.toDisplay(v.value)
                                                    : std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<std::pair<Micros, bool>> times{{main.range.start + in.view.cursorT, false}};
    if (in.view.datumT) {
        times.emplace_back(main.range.start + *in.view.datumT, true);
    }
    for (const auto& [t, datum] : times) {
        const double xv = valueAt(*x, t);
        for (std::size_t ti = 0; ti < frame.tiles.size(); ++ti) {
            std::vector<double> ys;
            for (const int tr : frame.traces) {
                if (frame.tiles[ti].trace >= 0 && tr != frame.tiles[ti].trace) {
                    continue;
                }
                const ScatterTrace& trace = body.traces[static_cast<std::size_t>(tr)];
                const ResolvedChannel* y = geom::channelOf(main, yIndex(tr), trace.yChannel);
                if (geom::readable(y)) {
                    ys.push_back(valueAt(*y, t + trace.yDelayUs));
                }
            }
            if (frame.average && !ys.empty()) {
                double sum = 0.0;
                int n = 0;
                for (const double v : ys) {
                    if (std::isfinite(v)) {
                        sum += v;
                        ++n;
                    }
                }
                ys = {n > 0 ? sum / n : std::numeric_limits<double>::quiet_NaN()};
            }
            for (const double yv : ys) {
                out.push_back(ScatterCursorPoint{static_cast<int>(ti), xv, yv, datum});
            }
        }
    }
    return out;
}

void emitScatterDynamic(DrawList& list, const ComponentBuildInputs& in, const ScatterBody& body,
                        const ScatterFrame& frame, const std::vector<ScatterCursorPoint>& points) {
    if (body.cursorStyle == "none") {
        return;
    }
    const float dpr = geom::dprOf(in);
    const geom::Colours colours(geom::schemeOf(in));
    const float lineW = geom::lineWidthOf(dpr);
    const float ring = (0.5F * body.pointSize + 3.0F) * dpr;
    for (const ScatterCursorPoint& p : points) {
        if (p.tile < 0 || static_cast<std::size_t>(p.tile) >= frame.tiles.size()) {
            continue;
        }
        const PlotTile& tile = frame.tiles[static_cast<std::size_t>(p.tile)];
        const float px = xPxOf(tile, p.x);
        const float py = yPxOf(tile, p.y);
        if (!std::isfinite(px) || !std::isfinite(py) || !inside(tile.plot, px, py)) {
            continue;
        }
        if (p.datum) { // hollow square, read-only
            const float s = ring;
            list.rectOutline(RectPx{px - s, py - s, 2.0F * s, 2.0F * s}, colours.datum, lineW);
            continue;
        }
        if (body.cursorStyle == "cross") {
            const float sx = std::round(px) + 0.5F;
            const float sy = std::round(py) + 0.5F;
            const std::array<float, 8> cross{
                sx, tile.plot.y, sx, tile.plot.y + tile.plot.h, tile.plot.x, sy, tile.plot.x + tile.plot.w,
                sy};
            list.lines(cross.data(), 4, colours.cursor, lineW);
            continue;
        }
        const std::vector<float> xy = ringOf(px, py, ring);
        list.polyline(xy.data(), xy.size() / 2, colours.cursor, 1.5F * lineW);
    }
}

} // namespace detail

// ---- builders

namespace {

ScatterBody bodyOf(const ComponentBuildInputs& in) {
    Result<ScatterBody> body = ScatterBody::parse(in.spec.body);
    return body ? std::move(*body) : ScatterBody{};
}

} // namespace

Result<ComponentBuild> ScatterGeometry::buildStatic(const ComponentBuildInputs& inputs,
                                                    const CancelToken& cancel) {
    Result<ScatterBody> parsed = ScatterBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const ScatterBody& body = *parsed;
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    build.list = geom::newList(inputs);
    detail::ScatterFrame frame = detail::scatterFrame(inputs, body);
    detail::ScatterScene scene;
    // Overlays n..1, Overlay 0, then Main: Main draws on top.
    for (std::size_t k = inputs.slots.size(); k-- > 0;) {
        Result<std::vector<detail::ScatterSet>> sets = detail::gatherSlot(inputs, body, frame, k, cancel);
        if (!sets) {
            return Failure{sets.error()};
        }
        for (detail::ScatterSet& s : *sets) {
            scene.sets.push_back(std::move(s));
        }
        if (cancel.checkpoint()) {
            return fail(ErrorCode::Cancelled, "scatter build cancelled");
        }
    }
    detail::fitScatterScales(frame, scene);
    // Trends, interval averages, density.
    const bool trends = body.style != "points" && body.trend.type != analysis::TrendType::None;
    std::uint64_t total = 0;
    for (detail::ScatterSet& set : scene.sets) {
        total += set.series.x.size();
        if (trends) {
            Result<analysis::TrendResult> fit =
                analysis::TrendFit::fit(set.series.x, set.series.y, body.trend);
            if (fit) {
                if (set.slot == 0 && body.showEquation) {
                    set.equation = analysis::TrendFit::equationText(*fit, body.significantDigits);
                }
                set.trend = std::move(*fit);
            }
        }
        if (body.intervalAverage && body.intervalBinWidth && !inputs.slots.empty()) {
            const ResolvedChannel* x = geom::channelOf(inputs.slots[set.slot], 0, body.xChannel);
            const std::optional<double> width = geom::unitValueIn(
                *body.intervalBinWidth, x != nullptr ? x->displayUnit : std::string{}, x, inputs.units.get());
            if (width && *width > 0.0) {
                Result<analysis::IntervalAverage> iv =
                    analysis::ScatterBinner::intervalAverage(set.series.x, set.series.y, *width);
                if (iv) {
                    set.interval = std::move(*iv);
                }
            }
        }
    }
    scene.density = body.densityAuto && total > body.densityThreshold;
    if (scene.density) {
        for (detail::ScatterSet& set : scene.sets) {
            const detail::PlotTile& tile = frame.tiles[static_cast<std::size_t>(std::max(set.tile, 0))];
            set.density =
                analysis::ScatterBinner::density(set.series, body.densityBinsX, body.densityBinsY,
                                                 tile.x.vMin, tile.x.vMax, tile.y.vMin, tile.y.vMax);
        }
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "scatter build cancelled");
    }
    detail::emitScatterStatic(build, inputs, body, frame, scene);
    return build;
}

Result<std::vector<ScatterExportSet>> ScatterGeometry::exportSets(const ComponentBuildInputs& inputs,
                                                                  const CancelToken& cancel) {
    Result<ScatterBody> parsed = ScatterBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const detail::ScatterFrame frame = detail::scatterFrame(inputs, *parsed);
    std::vector<ScatterExportSet> out;
    for (std::size_t k = 0; k < inputs.slots.size(); ++k) { // Main, Overlay 0, Overlays 1..n
        Result<std::vector<detail::ScatterSet>> sets = detail::gatherSlot(inputs, *parsed, frame, k, cancel);
        if (!sets) {
            return Failure{sets.error()};
        }
        for (detail::ScatterSet& s : *sets) {
            ScatterExportSet e;
            e.trace = s.trace;
            e.slot = k;
            e.x = std::move(s.series.x);
            e.y = std::move(s.series.y);
            e.z = std::move(s.series.z);
            out.push_back(std::move(e));
        }
        if (cancel.checkpoint()) {
            return fail(ErrorCode::Cancelled, "scatter export cancelled");
        }
    }
    return out;
}

std::vector<ScatterAxes> ScatterGeometry::axes(const ComponentBuildInputs& inputs) {
    const detail::ScatterFrame frame = detail::scatterFrame(inputs, bodyOf(inputs));
    std::vector<ScatterAxes> out;
    out.reserve(frame.tiles.size());
    for (const detail::PlotTile& tile : frame.tiles) {
        ScatterAxes a;
        a.plot = tile.plot;
        a.trace = tile.trace;
        a.xMin = tile.x.vMin;
        a.xMax = tile.x.vMax;
        a.yMin = tile.y.vMin;
        a.yMax = tile.y.vMax;
        out.push_back(a);
    }
    return out;
}

DrawList ScatterGeometry::buildDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    const ScatterBody body = bodyOf(inputs);
    const detail::ScatterFrame frame = detail::scatterFrame(inputs, body);
    detail::emitScatterDynamic(list, inputs, body, frame, detail::scatterCursorPoints(inputs, body, frame));
    return list;
}

DrawList ScatterGeometry::buildChrome(const ComponentBuildInputs& inputs, bool pending) {
    DrawList list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const ScatterBody body = bodyOf(inputs);
    const RectPx caption = geom::captionRect(inputs);
    std::string ys;
    for (const ScatterTrace& t : body.traces) {
        if (t.visible && !t.yChannel.empty()) {
            ys += (ys.empty() ? "" : ", ") + t.yChannel;
        }
    }
    std::string autoText = ys.empty() ? body.xChannel : ys + " vs " + body.xChannel;
    geom::emitCaption(list, caption, geom::captionText(inputs, autoText), colours, dpr);
    if (pending || geom::anyPending(inputs)) {
        geom::emitPendingStripe(
            list, RectPx{0.0F, caption.h, static_cast<float>(inputs.layout.widthPx), 0.0F}, colours, dpr);
    }
    geom::emitFocus(list, inputs, colours, dpr);
    return list;
}

} // namespace esses::plot
