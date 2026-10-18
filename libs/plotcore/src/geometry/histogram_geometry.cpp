// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HistogramBody and HistogramGeometry (04 section 11; plot_scenes.hpp has the frame / scene / emit split).
// Layout: caption 18; the colour legend on the right when K is on and a colour channel is set; one tile
// (overlapped) or one per visible trace (tiled). The bins (BandAxis, generated in the active channel's
// display unit with the Main source's whole-column SummaryPyramid range as the automatic data limits) are
// equal-width slots along x, labelled at their boundaries every binLabelStep (condensedAxes: only the bottom
// row of tiles); the y scale is manual or 0 to the largest value of the tile's sets. Styles: bar (Main
// filled, split into colour-band sub-bars with a colour channel; overlays outlined beneath Main), line
// (through the bin centres; stackLines lifts trace n by n x 12 logical px) and text (a table: bins x sets).
// HitKind::Bin per slot; the Dynamic layer highlights the bin of the active channel's value at the cursor.

#include "body_json.hpp"
#include "geometry_common.hpp"
#include "plot_scenes.hpp"

#include <esses/store/resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <utility>

namespace esses::plot {

namespace {

using nlohmann::json;

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

// ---- HistogramBody

Result<HistogramBody> HistogramBody::parse(const JsonDoc& body) {
    return jsonio::guardJson([&]() -> Result<HistogramBody> {
        HistogramBody out;
        out.rest = body;
        if (body.isNull()) {
            return out;
        }
        const json j = jsonio::fromDoc(body);
        if (!j.is_object()) {
            jsonio::violate("", "histogram body is not an object");
        }
        jsonio::ObjectReader r(j, "");
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
                HistogramTrace trace;
                trace.channel = jsonio::asString(t.required("channel"), t.child("channel"));
                if (const json* v = t.optional("visible")) {
                    trace.visible = jsonio::asBool(*v, t.child("visible"));
                }
                out.traces.push_back(std::move(trace));
            }
        }
        if (const json* v = r.optional("bins")) {
            out.bins = bandAxisOf(*v, r.child("bins"));
        }
        if (const json* v = r.optional("yMode")) {
            out.yMode = enumOf(*v, r.child("yMode"), {"count", "percent", "time"});
        }
        if (const json* v = r.optional("yScale")) {
            out.yScale = detail::readScaleSpec(*v, r.child("yScale"));
        }
        if (const json* v = r.optional("excludeUntrusted")) {
            out.excludeUntrusted = jsonio::asBool(*v, r.child("excludeUntrusted"));
        }
        if (const json* v = r.optional("stackLines")) {
            out.stackLines = jsonio::asBool(*v, r.child("stackLines"));
        }
        if (const json* v = r.optional("style")) {
            out.style = enumOf(*v, r.child("style"), {"bar", "line", "text"});
        }
        if (const json* v = r.optional("layout")) {
            out.layout = enumOf(*v, r.child("layout"), {"overlapped", "tiled"});
        }
        if (const json* v = r.optional("tileCols")) {
            out.tileCols = jsonio::asInt32(*v, r.child("tileCols"));
        }
        if (const json* v = r.optional("binLabelStep")) {
            out.binLabelStep = jsonio::asInt32(*v, r.child("binLabelStep"));
        }
        if (const json* v = r.optional("condensedAxes")) {
            out.condensedAxes = jsonio::asBool(*v, r.child("condensedAxes"));
        }
        if (const json* colour = r.optional("colourChannel")) {
            if (!colour->is_object()) {
                jsonio::violate(r.child("colourChannel"), "expected an object");
            }
            jsonio::ObjectReader c(*colour, r.child("colourChannel"));
            if (const json* v = c.optional("channel")) {
                out.colourChannel = jsonio::asString(*v, c.child("channel"));
            }
            if (const json* v = c.optional("axis")) { // null: no colour axis (optional() skips null)
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
        return out;
    });
}

JsonDoc HistogramBody::toJson() const {
    const HistogramBody d;
    json j = detail::mergeOver(rest);
    if (!traces.empty() || j.contains("traces")) {
        const json old = j.contains("traces") && j["traces"].is_array() ? j["traces"] : json::array();
        json arr = json::array();
        for (std::size_t i = 0; i < traces.size(); ++i) {
            json tj = i < old.size() && old[i].is_object() ? old[i] : json::object();
            tj["channel"] = traces[i].channel;
            detail::put(tj, "visible", traces[i].visible, true);
            arr.push_back(std::move(tj));
        }
        j["traces"] = std::move(arr);
    }
    if (j.contains("bins") || !(bins == d.bins)) {
        const json old = j.contains("bins") ? j["bins"] : json();
        j["bins"] = detail::bandAxisJson(old.is_object() ? &old : nullptr, bins);
    }
    detail::put(j, "yMode", yMode, d.yMode);
    if (j.contains("yScale") || !(yScale == d.yScale)) {
        j["yScale"] = detail::scaleSpecJson(yScale);
    }
    detail::put(j, "excludeUntrusted", excludeUntrusted, d.excludeUntrusted);
    detail::put(j, "stackLines", stackLines, d.stackLines);
    detail::put(j, "style", style, d.style);
    detail::put(j, "layout", layout, d.layout);
    detail::put(j, "tileCols", tileCols, d.tileCols);
    detail::put(j, "binLabelStep", binLabelStep, d.binLabelStep);
    detail::put(j, "condensedAxes", condensedAxes, d.condensedAxes);
    json colour = detail::childObject(j, "colourChannel");
    detail::put(colour, "channel", colourChannel, d.colourChannel);
    if (colourAxis) {
        const json old = colour.contains("axis") ? colour["axis"] : json();
        colour["axis"] = detail::bandAxisJson(old.is_object() ? &old : nullptr, *colourAxis);
    } else if (colour.contains("axis")) {
        colour["axis"] = nullptr;
    }
    detail::putObject(j, "colourChannel", std::move(colour));
    json gating = detail::childObject(j, "gating");
    detail::put(gating, "channel", gatingChannel, d.gatingChannel);
    detail::putObject(j, "gating", std::move(gating));
    return detail::docOf(j, rest);
}

std::vector<ChannelUse> HistogramBody::channels() const {
    std::vector<ChannelUse> out;
    out.reserve(traces.size() + 2);
    for (const HistogramTrace& t : traces) {
        out.push_back(ChannelUse{t.channel, {}, std::nullopt});
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

constexpr float kStackLogical = 12.0F;

std::size_t zIndex(const HistogramBody& body) {
    return body.traces.size();
}
std::size_t gIndex(const HistogramBody& body) {
    return body.traces.size() + (body.colourChannel.empty() ? 0 : 1);
}

analysis::HistogramYMode yModeOf(const std::string& s) {
    if (s == "count") {
        return analysis::HistogramYMode::Count;
    }
    return s == "time" ? analysis::HistogramYMode::Time : analysis::HistogramYMode::Percent;
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

// The active trace: the component's active channel when it is a visible trace, else the first visible.
int activeTrace(const ComponentBuildInputs& in, const HistogramBody& body, const std::vector<int>& traces) {
    for (const int t : traces) {
        if (body.traces[static_cast<std::size_t>(t)].channel == in.spec.activeChannel) {
            return t;
        }
    }
    return traces.empty() ? -1 : traces.front();
}

// base -> `unit`: the channel's display converter when the units agree, else through the registry.
units::Converter converterTo(const ResolvedChannel& rc, const std::string& unit,
                             const units::UnitRegistry* registry) {
    if (rc.displayUnit == unit || registry == nullptr) {
        return rc.toDisplay;
    }
    if (const std::optional<units::UnitId> id = registry->find(unit)) {
        return registry->toBase(*id).inverse();
    }
    return rc.toDisplay;
}

std::string valueText(double v, analysis::HistogramYMode mode) {
    switch (mode) {
    case analysis::HistogramYMode::Count:
        return geom::formatValue(v, 0);
    case analysis::HistogramYMode::Percent:
        return geom::formatValue(v, 1);
    case analysis::HistogramYMode::Time:
        return geom::formatValue(v, 2);
    }
    return geom::formatValue(v, 1);
}

// The value of boundary j (between bin j - 1 and bin j; 0 = left edge, n = right edge); NaN at an open end.
double boundaryValue(const std::vector<analysis::Band>& bins, std::size_t j) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (j < bins.size()) {
        const analysis::Band& b = bins[j];
        if (b.lessMin) {
            return nan;
        }
        if (b.greaterMax) {
            return j > 0 ? b.lo : nan;
        }
        return b.lo;
    }
    if (bins.empty() || bins.back().greaterMax) {
        return nan;
    }
    return bins.back().hi;
}

double binStep(const std::vector<analysis::Band>& bins) {
    for (const analysis::Band& b : bins) {
        if (!b.lessMin && !b.greaterMax && b.hi > b.lo) {
            return b.hi - b.lo;
        }
    }
    return 0.0;
}

std::string binLabel(const analysis::Band& b, int decimals) {
    if (b.lessMin) {
        return "< " + geom::formatValue(b.hi, decimals);
    }
    if (b.greaterMax) {
        return ">= " + geom::formatValue(b.lo, decimals);
    }
    return geom::formatValue(b.lo, decimals) + " .. " + geom::formatValue(b.hi, decimals);
}

} // namespace

HistogramFrame histogramFrame(const ComponentBuildInputs& in, const HistogramBody& body) {
    HistogramFrame f;
    const float dpr = geom::dprOf(in);
    const ITextMetrics& metrics = geom::metricsOf(in);
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    f.caption = geom::captionRect(in);
    f.yMode = yModeOf(body.yMode);
    for (std::size_t i = 0; i < body.traces.size(); ++i) {
        if (body.traces[i].visible && !body.traces[i].channel.empty()) {
            f.traces.push_back(static_cast<int>(i));
        }
    }
    const SlotData* main = in.slots.empty() ? nullptr : &in.slots.front();
    const int active = activeTrace(in, body, f.traces);
    const ResolvedChannel* activeRc =
        main != nullptr && active >= 0
            ? geom::channelOf(*main, static_cast<std::size_t>(active),
                              body.traces[static_cast<std::size_t>(active)].channel)
            : nullptr;
    if (geom::readable(activeRc)) {
        f.binUnit = activeRc->displayUnit;
        const geom::SummaryRange sr = geom::summaryRange(*activeRc, activeRc->toDisplay);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        Result<std::vector<analysis::Band>> bins =
            analysis::generateBands(body.bins, sr.range.valid() ? sr.range.vMin : nan,
                                    sr.range.valid() ? sr.range.vMax : nan, in.units.get(), f.binUnit);
        if (bins) {
            f.bins = std::move(*bins);
        }
    }
    const ResolvedChannel* zRc = main != nullptr && !body.colourChannel.empty()
                                     ? geom::channelOf(*main, zIndex(body), body.colourChannel)
                                     : nullptr;
    if (body.colourAxis && geom::readable(zRc)) {
        const geom::SummaryRange sr = geom::summaryRange(*zRc, zRc->toDisplay);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        Result<std::vector<analysis::Band>> bands =
            analysis::generateBands(*body.colourAxis, sr.range.valid() ? sr.range.vMin : nan,
                                    sr.range.valid() ? sr.range.vMax : nan, in.units.get(), zRc->displayUnit);
        if (bands) {
            f.colourBands = std::move(*bands);
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
    const char* yTitle = f.yMode == analysis::HistogramYMode::Count
                             ? "Count"
                             : (f.yMode == analysis::HistogramYMode::Time ? "Time [s]" : "Percent [%]");
    for (std::size_t t = 0; t < f.tiles.size(); ++t) {
        PlotTile& tile = f.tiles[t];
        tile.trace = tiled ? f.traces[t] : -1;
        std::string xTitle;
        for (const int tr : f.traces) {
            if (tiled && tr != tile.trace) {
                continue;
            }
            const std::string& name = body.traces[static_cast<std::size_t>(tr)].channel;
            const ResolvedChannel* rc =
                main != nullptr ? geom::channelOf(*main, static_cast<std::size_t>(tr), name) : nullptr;
            xTitle += (xTitle.empty() ? "" : ", ") + titleOf(name, rc);
        }
        tile.xTitleText = xTitle;
        tile.yTitleText = yTitle;
        if (body.yScale.mode == ScaleMode::Manual && body.yScale.min && body.yScale.max) {
            tile.y.unite(body.yScale.min->v, body.yScale.max->v);
            tile.y.widenDegenerate();
        }
    }
    return f;
}

void fitHistogramScales(HistogramFrame& frame, const HistogramBody& body, const HistogramScene& scene) {
    static_cast<void>(body);
    for (std::size_t t = 0; t < frame.tiles.size(); ++t) {
        PlotTile& tile = frame.tiles[t];
        if (tile.y.valid()) {
            continue;
        }
        double peak = 0.0;
        for (const HistogramSet& set : scene.sets) {
            if (set.tile != static_cast<int>(t)) {
                continue;
            }
            for (const double v : set.result.values) {
                if (std::isfinite(v)) {
                    peak = std::max(peak, v);
                }
            }
        }
        tile.y.unite(0.0, peak > 0.0 ? peak : 1.0);
    }
}

void emitHistogramStatic(ComponentBuild& build, const ComponentBuildInputs& in, const HistogramBody& body,
                         const HistogramFrame& frame, const HistogramScene& scene) {
    DrawList& list = build.list;
    const float dpr = geom::dprOf(in);
    const project::Scheme& scheme = geom::schemeOf(in);
    const geom::Colours colours(scheme);
    const ITextMetrics& metrics = geom::metricsOf(in);
    const float lineW = geom::lineWidthOf(dpr);
    list.clear(colours.background);
    const bool text = body.style == "text";
    const std::size_t nb = frame.bins.size();
    const int decimals = geom::decimalsForStep(binStep(frame.bins));
    const int cols =
        frame.tiles.empty()
            ? 1
            : static_cast<int>(std::count_if(frame.tiles.begin(), frame.tiles.end(), [&](const PlotTile& t) {
                  return t.bounds.y == frame.tiles.front().bounds.y;
              }));
    // Main in its channel colours, the overlays in their range key colours (04 section 11.1 item 8, 06
    // section 5.3 item 3).
    const auto setColour = [&](const HistogramSet& set) {
        const SlotData& slot = in.slots[set.slot];
        if (slot.slot.kind != SlotKind::Main) {
            return rangeKeyColour(scheme, slot.slot);
        }
        const ResolvedChannel* rc = geom::channelOf(slot, static_cast<std::size_t>(set.trace),
                                                    body.traces[static_cast<std::size_t>(set.trace)].channel);
        return traceColour(scheme, in.selection, slot.slot, colourIndexOf(rc, set.trace));
    };
    for (std::size_t ti = 0; ti < frame.tiles.size(); ++ti) {
        PlotTile tile = frame.tiles[ti];
        if (text) {
            tile.y = geom::Scale{};
        }
        emitTileAxes(list, build.hits, tile, static_cast<int>(ti), !text, false, colours, metrics, dpr);
        const RectPx& plot = tile.plot;
        if (!(plot.w > 0.0F) || !(plot.h > 0.0F) || nb == 0) {
            continue;
        }
        const float slotW = plot.w / static_cast<float>(nb);
        const auto slotX = [&](std::size_t j) { return plot.x + slotW * static_cast<float>(j); };
        const bool lastRow = ti + static_cast<std::size_t>(cols) >= frame.tiles.size();
        // Bin boundary ticks and labels.
        if (!text && (!body.condensedAxes || lastRow)) {
            std::vector<float> ticks;
            float lastRight = -std::numeric_limits<float>::infinity();
            const int step = std::max(1, body.binLabelStep);
            for (std::size_t j = 0; j <= nb; ++j) {
                const double v = boundaryValue(frame.bins, j);
                const float x = std::round(slotX(j)) + 0.5F;
                ticks.insert(ticks.end(), {x, tile.xAxis.y, x, tile.xAxis.y + 3.0F * dpr});
                if (!std::isfinite(v) || j % static_cast<std::size_t>(step) != 0) {
                    continue;
                }
                const std::string label = geom::formatValue(v, decimals);
                const float half = 0.5F * metrics.advancePx(TextStyleId::Axis, label, dpr);
                if (slotX(j) - half < lastRight + 2.0F * dpr) {
                    continue;
                }
                list.textRun(label, slotX(j), tile.xAxis.y + 4.0F * dpr, TextStyleId::Axis,
                             TextAnchor::TopCentre, colours.foreground);
                lastRight = slotX(j) + half;
            }
            list.lines(ticks.data(), ticks.size() / 2, colours.foreground, lineW);
        }
        if (text) {
            // The table: a header row, then one row per bin; columns: the bin, then the sets (Main first).
            std::vector<const HistogramSet*> sets;
            for (const HistogramSet& set : scene.sets) {
                if (set.tile == static_cast<int>(ti)) {
                    sets.push_back(&set);
                }
            }
            std::stable_sort(sets.begin(), sets.end(),
                             [](const HistogramSet* a, const HistogramSet* b) { return a->slot < b->slot; });
            const float rowH = metrics.lineHeightPx(TextStyleId::Table, dpr);
            const float colW = plot.w / static_cast<float>(sets.size() + 1);
            const float pad = 3.0F * dpr;
            float y = plot.y + pad;
            list.scissor(plot);
            list.textRun("Bin", plot.x + pad, y, TextStyleId::Table, TextAnchor::TopLeft, colours.foreground2,
                         colW - 2.0F * pad);
            for (std::size_t c = 0; c < sets.size(); ++c) {
                const HistogramSet& set = *sets[c];
                std::string head = body.traces[static_cast<std::size_t>(set.trace)].channel;
                if (set.slot > 0) {
                    head += " (" + in.slots[set.slot].slot.range.name + ")";
                }
                list.textRun(head, plot.x + colW * static_cast<float>(c + 2) - pad, y, TextStyleId::Table,
                             TextAnchor::TopRight, setColour(set), colW - 2.0F * pad);
            }
            y += rowH;
            for (std::size_t j = 0; j < nb && y + rowH <= plot.y + plot.h; ++j) {
                list.textRun(binLabel(frame.bins[j], decimals), plot.x + pad, y, TextStyleId::Table,
                             TextAnchor::TopLeft, colours.foreground, colW - 2.0F * pad);
                for (std::size_t c = 0; c < sets.size(); ++c) {
                    const HistogramSet& set = *sets[c];
                    if (j < set.result.values.size()) {
                        list.textRun(valueText(set.result.values[j], frame.yMode),
                                     plot.x + colW * static_cast<float>(c + 2) - pad, y, TextStyleId::Table,
                                     TextAnchor::TopRight, setColour(set), colW - 2.0F * pad);
                    }
                }
                y += rowH;
            }
            list.scissor(RectPx{});
        } else if (tile.y.valid()) {
            const float base = std::clamp(yPxOf(tile, 0.0), plot.y, plot.y + plot.h);
            const float inset = slotW > 4.0F * dpr ? std::round(dpr) : 0.0F;
            const auto yOf = [&](double v) { return std::clamp(yPxOf(tile, v), plot.y, plot.y + plot.h); };
            list.scissor(plot);
            for (const HistogramSet& set : scene.sets) {
                if (set.tile != static_cast<int>(ti)) {
                    continue;
                }
                const Rgba colour = setColour(set);
                if (body.style == "line") {
                    // stackLines lifts the n-th trace of an overlapped tile (a tiled tile holds one trace).
                    const std::ptrdiff_t order =
                        tile.trace >= 0 ? std::ptrdiff_t{0}
                                        : std::find(frame.traces.begin(), frame.traces.end(), set.trace) -
                                              frame.traces.begin();
                    const float lift =
                        body.stackLines ? kStackLogical * dpr * static_cast<float>(order) : 0.0F;
                    std::vector<float> line;
                    for (std::size_t j = 0; j < nb && j < set.result.values.size(); ++j) {
                        line.insert(line.end(), {slotX(j) + 0.5F * slotW, yOf(set.result.values[j]) - lift});
                    }
                    if (line.size() >= 4) {
                        list.polyline(line.data(), line.size() / 2, colour,
                                      1.5F * lineW * traceWidthScale(scheme, in.slots[set.slot].slot),
                                      traceDash(scheme, in.slots[set.slot].slot));
                    }
                    continue;
                }
                for (std::size_t j = 0; j < nb && j < set.result.values.size(); ++j) {
                    const float top = yOf(set.result.values[j]);
                    const RectPx bar{slotX(j) + inset, top, std::max(0.0F, slotW - 2.0F * inset), base - top};
                    if (!(bar.h > 0.0F) || !(bar.w > 0.0F)) {
                        continue;
                    }
                    if (set.slot > 0) { // overlays: outlined bars
                        list.rectOutline(bar, colour, lineW);
                        continue;
                    }
                    const bool split = j < set.result.colourSplit.size() &&
                                       set.result.colourSplit[j].size() == frame.colourBands.size() &&
                                       !frame.colourBands.empty();
                    if (!split) {
                        list.filledRect(bar, colour);
                        continue;
                    }
                    double acc = 0.0;
                    for (std::size_t b = 0; b < frame.colourBands.size(); ++b) {
                        const double part = set.result.colourSplit[j][b];
                        if (!(part > 0.0)) {
                            continue;
                        }
                        const float y0 = yOf(acc + part);
                        const float y1 = yOf(acc);
                        acc += part;
                        if (y1 > y0) {
                            list.filledRect(RectPx{bar.x, y0, bar.w, y1 - y0},
                                            static_cast<Rgba>(frame.colourBands[b].colour));
                        }
                    }
                    if (acc < set.result.values[j]) { // samples outside every colour band
                        const float y0 = yOf(set.result.values[j]);
                        const float y1 = yOf(acc);
                        if (y1 > y0) {
                            list.filledRect(RectPx{bar.x, y0, bar.w, y1 - y0}, colour);
                        }
                    }
                }
            }
            list.scissor(RectPx{});
        }
        // Bin hit regions over the plot.
        for (std::size_t j = 0; j < nb; ++j) {
            HitResult hit;
            hit.kind = HitKind::Bin;
            hit.bin = static_cast<int>(j);
            hit.group = static_cast<int>(ti);
            hit.trace = tile.trace;
            build.hits.addRect(RectPx{slotX(j), plot.y, slotW, plot.h}, hit);
        }
    }
    emitBandLegend(list, frame.legend, frame.colourBands, colours, metrics, dpr);
}

std::optional<std::size_t> histogramCursorBin(const ComponentBuildInputs& in, const HistogramBody& body,
                                              const HistogramFrame& frame) {
    if (in.slots.empty() || frame.bins.empty()) {
        return std::nullopt;
    }
    const int active = activeTrace(in, body, frame.traces);
    if (active < 0) {
        return std::nullopt;
    }
    const SlotData& main = in.slots.front();
    const ResolvedChannel* rc = geom::channelOf(main, static_cast<std::size_t>(active),
                                                body.traces[static_cast<std::size_t>(active)].channel);
    if (!geom::readable(rc)) {
        return std::nullopt;
    }
    const std::optional<store::Series>& series = rc->series;
    const std::optional<model::ChannelDescriptor>& descriptor = rc->descriptor;
    if (!series || !descriptor) {
        return std::nullopt;
    }
    const store::ValueSample v =
        store::Resampler::sampleAt(*series, main.range.start + in.view.cursorT, descriptor->interp);
    if (v.state != store::SampleState::Valid) {
        return std::nullopt;
    }
    return analysis::HistogramBinner::binOf(converterTo(*rc, frame.binUnit, in.units.get())(v.value),
                                            frame.bins);
}

void emitHistogramDynamic(DrawList& list, const ComponentBuildInputs& in, const HistogramBody& body,
                          const HistogramFrame& frame, std::optional<std::size_t> bin) {
    if (!bin || *bin >= frame.bins.size() || body.style == "text") {
        return;
    }
    const float dpr = geom::dprOf(in);
    const geom::Colours colours(geom::schemeOf(in));
    const int active = activeTrace(in, body, frame.traces);
    for (const PlotTile& tile : frame.tiles) {
        if (tile.trace >= 0 && tile.trace != active) {
            continue;
        }
        const float slotW = tile.plot.w / static_cast<float>(frame.bins.size());
        const RectPx r{tile.plot.x + slotW * static_cast<float>(*bin), tile.plot.y, slotW, tile.plot.h};
        if (r.w > 0.0F && r.h > 0.0F) {
            list.filledRect(r, geom::withAlpha(colours.cursor, 0x40));
            list.rectOutline(r, colours.cursor, geom::lineWidthOf(dpr));
        }
    }
}

} // namespace detail

// ---- builders

namespace {

HistogramBody bodyOf(const ComponentBuildInputs& in) {
    Result<HistogramBody> body = HistogramBody::parse(in.spec.body);
    return body ? std::move(*body) : HistogramBody{};
}

} // namespace

namespace {

// The scene of a histogram: HistogramBinner per visible slot and trace in draw order (overlays n..1, Overlay
// 0, Main; within a slot the active trace last). Cancelled and Unsupported fail; other failures drop a set.
Result<detail::HistogramScene> sceneOf(const ComponentBuildInputs& inputs, const HistogramBody& body,
                                       const detail::HistogramFrame& frame, const CancelToken& cancel) {
    detail::HistogramScene scene;
    if (frame.bins.empty()) {
        return scene;
    }
    analysis::HistogramRequest request;
    request.bins = frame.bins;
    request.yMode = frame.yMode;
    request.excludeUntrusted = body.excludeUntrusted;
    const int active = detail::activeTrace(inputs, body, frame.traces);
    for (std::size_t k = inputs.slots.size(); k-- > 0;) {
        const SlotData& slot = inputs.slots[k];
        const std::vector<analysis::SampleSpan> spans = geom::visibleSpans(inputs, slot);
        if (spans.empty()) {
            continue;
        }
        std::vector<int> order;
        for (const int t : frame.traces) {
            if (t != active) {
                order.push_back(t);
            }
        }
        if (active >= 0) {
            order.push_back(active);
        }
        for (const int t : order) {
            const ResolvedChannel* rc = geom::channelOf(slot, static_cast<std::size_t>(t),
                                                        body.traces[static_cast<std::size_t>(t)].channel);
            // readable() implies a series; testing it again shows clang-tidy that the accesses are checked.
            if (!geom::readable(rc) || !rc->series) {
                continue;
            }
            analysis::HistogramInput input;
            input.series = *rc->series;
            input.toBins = detail::converterTo(*rc, frame.binUnit, inputs.units.get());
            if (!body.gatingChannel.empty()) {
                const ResolvedChannel* g = geom::channelOf(slot, detail::gIndex(body), body.gatingChannel);
                if (geom::readable(g) && g->series) {
                    input.gating = *g->series;
                }
            }
            if (!body.colourChannel.empty() && !frame.colourBands.empty()) {
                const ResolvedChannel* z = geom::channelOf(slot, detail::zIndex(body), body.colourChannel);
                if (geom::readable(z) && z->series) {
                    input.colour = *z->series;
                    input.colourToBands = z->toDisplay;
                    input.colourBands = frame.colourBands;
                    input.colourInterp = detail::interpOf(*z); // D-194 (a)
                }
            }
            input.spans = spans;
            Result<analysis::HistogramResult> result =
                analysis::HistogramBinner::bin(input, request, Priority::Visible, cancel);
            if (!result) {
                if (result.error().code == ErrorCode::Cancelled ||
                    result.error().code == ErrorCode::Unsupported) {
                    return Failure{result.error()};
                }
                continue;
            }
            detail::HistogramSet set;
            set.trace = t;
            set.slot = k;
            if (body.layout == "tiled" && frame.tiles.size() > 1) {
                const auto it = std::find(frame.traces.begin(), frame.traces.end(), t);
                set.tile = static_cast<int>(it - frame.traces.begin());
            }
            set.result = std::move(*result);
            scene.sets.push_back(std::move(set));
        }
        if (cancel.checkpoint()) {
            return fail(ErrorCode::Cancelled, "histogram build cancelled");
        }
    }
    return scene;
}

} // namespace

Result<ComponentBuild> HistogramGeometry::buildStatic(const ComponentBuildInputs& inputs,
                                                      const CancelToken& cancel) {
    Result<HistogramBody> parsed = HistogramBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const HistogramBody& body = *parsed;
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    build.list = geom::newList(inputs);
    detail::HistogramFrame frame = detail::histogramFrame(inputs, body);
    Result<detail::HistogramScene> scene = sceneOf(inputs, body, frame, cancel);
    if (!scene) {
        return Failure{scene.error()};
    }
    detail::fitHistogramScales(frame, body, *scene);
    detail::emitHistogramStatic(build, inputs, body, frame, *scene);
    return build;
}

Result<HistogramExport> HistogramGeometry::exportTable(const ComponentBuildInputs& inputs,
                                                       const CancelToken& cancel) {
    Result<HistogramBody> parsed = HistogramBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const detail::HistogramFrame frame = detail::histogramFrame(inputs, *parsed);
    Result<detail::HistogramScene> scene = sceneOf(inputs, *parsed, frame, cancel);
    if (!scene) {
        return Failure{scene.error()};
    }
    HistogramExport out;
    out.bins = frame.bins;
    out.binUnit = frame.binUnit;
    out.yMode = parsed->yMode;
    for (std::size_t k = 0; k < inputs.slots.size(); ++k) { // Main first; body trace order within a slot
        for (const int t : frame.traces) {
            for (detail::HistogramSet& set : scene->sets) {
                if (set.slot == k && set.trace == t) {
                    out.columns.push_back(HistogramExportColumn{t, k, std::move(set.result.values)});
                }
            }
        }
    }
    return out;
}

DrawList HistogramGeometry::buildDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    const HistogramBody body = bodyOf(inputs);
    const detail::HistogramFrame frame = detail::histogramFrame(inputs, body);
    detail::emitHistogramDynamic(list, inputs, body, frame, detail::histogramCursorBin(inputs, body, frame));
    return list;
}

DrawList HistogramGeometry::buildChrome(const ComponentBuildInputs& inputs, bool pending) {
    DrawList list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const HistogramBody body = bodyOf(inputs);
    const RectPx caption = geom::captionRect(inputs);
    std::string autoText;
    for (const HistogramTrace& t : body.traces) {
        if (t.visible && !t.channel.empty()) {
            autoText += (autoText.empty() ? "" : ", ") + t.channel;
        }
    }
    geom::emitCaption(list, caption, geom::captionText(inputs, autoText), colours, dpr);
    if (pending || geom::anyPending(inputs)) {
        geom::emitPendingStripe(
            list, RectPx{0.0F, caption.h, static_cast<float>(inputs.layout.widthPx), 0.0F}, colours, dpr);
    }
    geom::emitFocus(list, inputs, colours, dpr);
    return list;
}

} // namespace esses::plot
