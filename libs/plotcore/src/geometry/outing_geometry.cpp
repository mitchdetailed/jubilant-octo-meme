// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OutingBody and OutingGeometry (04 section 9). The outing graph shares time_distance's groups/traces body
// (TimeDistanceBody keeps every other member in `rest`) and draws the whole Main outing in outing-relative x:
// time since the outing start, or metres from it through the Main DistanceChain re-origined at the outing
// start. Layout (logical px x dpr): caption 18, then the range band (14, BandLayout) over the trace area with
// one cell per lap of the Main outing (edges = lap boundaries; the Main lap tinted), the key column
// (KeyLayout, labels Static, values at the cursor Dynamic), a 36 px axis strip, the panels by `edges` (equal
// shares when every group keeps [0, 1]) and the x axis (18). Traces come from TraceBuilder::decimateSummary
// only (04 section 4.8: never pins chunks); with enableOverlays in time mode the visible overlays' outings
// are drawn aligned at lap start (x = t - k.start + main.start - outing.start), beneath Main. The zoom
// rectangle is Dynamic (zoomRect: the shared zoom shifted by -outingExtent().a); its handles are derived from
// zoomRect by the widget, since a Dynamic build carries no HitTester.

#include "body_json.hpp"
#include "geometry_common.hpp"

#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/band_layout.hpp>
#include <esses/plotcore/outing_geometry.hpp>
#include <esses/plotcore/trace_builder.hpp>
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace esses::plot {

namespace {

using nlohmann::json;

// ---- body and traces

OutingBody bodyOf(const ComponentBuildInputs& in) {
    Result<OutingBody> body = OutingBody::parse(in.spec.body);
    return body ? std::move(*body) : OutingBody{};
}

struct FlatTrace {
    int group = 0;
    int index = 0; // flat channel index (channelNames order)
    const TdTrace* trace = nullptr;
};

std::vector<FlatTrace> flatTraces(const TimeDistanceBody& body) {
    std::vector<FlatTrace> out;
    int index = 0;
    for (std::size_t g = 0; g < body.groups.size(); ++g) {
        for (const TdTrace& t : body.groups[g].traces) {
            out.push_back(FlatTrace{static_cast<int>(g), index++, &t});
        }
    }
    return out;
}

int colourIndexOf(const ResolvedChannel* rc, int flatIndex) {
    if (rc != nullptr && rc->descriptor && rc->descriptor->colourIndex >= 0) {
        return rc->descriptor->colourIndex;
    }
    return flatIndex;
}

std::string labelOf(const TdTrace& trace, const ResolvedChannel* main) {
    if (main == nullptr || !main->descriptor) {
        return trace.channel + " (missing)";
    }
    return main->displayUnit.empty() ? trace.channel : trace.channel + " [" + main->displayUnit + "]";
}

int dpsOf(const TdTrace& trace, const ResolvedChannel* rc) {
    if (trace.dps) {
        return *trace.dps;
    }
    return rc != nullptr ? rc->dps : 2;
}

std::string valueTemplate(int dps) {
    std::string s = "-88888";
    if (dps > 0) {
        s += '.';
        s.append(static_cast<std::size_t>(std::min(dps, 6)), '8');
    }
    return s;
}

std::string sourceName(const SlotData& slot) {
    if (slot.source && !slot.source->prov.path.empty()) {
        const std::u8string u8 = slot.source->prov.path.filename().u8string();
        return {u8.begin(), u8.end()};
    }
    return slot.slot.range.name;
}

// ---- outing coordinates

// The slots drawn: Main, plus every other visible slot with enableOverlays in time mode.
std::vector<std::size_t> drawnSlots(const ComponentBuildInputs& in, const OutingBody& body) {
    std::vector<std::size_t> out;
    if (in.slots.empty()) {
        return out;
    }
    out.push_back(0);
    if (body.enableOverlays && in.view.domain == Domain::Time) {
        for (std::size_t k = 1; k < in.slots.size(); ++k) {
            out.push_back(k);
        }
    }
    return out;
}

// The Main mapper of the worksheet (alignment offsets excluded: the shared zoom does not fold them in).
DomainMapper worksheetMainMapper(const ComponentBuildInputs& in) {
    const SlotData& main = in.slots.front();
    RangeGeom range = main.range;
    range.offsetUs = 0;
    range.offsetM = 0.0;
    return {in.view.domain, range, main.outing, main.chain, Rate{}, 0};
}

// x of the outing graph = x of the worksheet Main mapper - origin (outingExtent().a).
double outingOrigin(const ComponentBuildInputs& in) {
    return worksheetMainMapper(in).outingExtent().a;
}

// The mapper of slot k in outing coordinates (see the file comment).
DomainMapper outingMapper(const ComponentBuildInputs& in, std::size_t k, const store::Series* series) {
    const SlotData& main = in.slots.front();
    const SlotData& slot = in.slots[k];
    Rate rate;
    Micros t0 = 0;
    if (series != nullptr) {
        if (const auto* u = std::get_if<store::UniformSeries>(series)) {
            rate = u->rate;
            t0 = u->t0;
        }
    }
    if (in.view.domain == Domain::Distance) {
        std::shared_ptr<const DistanceChain> chain;
        if (main.chain != nullptr) {
            chain = std::make_shared<const DistanceChain>(main.chain->pieces(), main.outing.start);
        }
        return {
            Domain::Distance, RangeGeom{main.outing.start, main.outing.end}, main.outing, chain, rate, t0};
    }
    if (k == 0) {
        return {Domain::Time, RangeGeom{main.outing.start, main.outing.end}, main.outing, nullptr, rate, t0};
    }
    RangeGeom aligned{slot.range.start, slot.range.end};
    aligned.offsetUs = main.range.start - main.outing.start;
    return {Domain::Time, aligned, slot.outing, nullptr, rate, t0};
}

// The outing span in outing coordinates.
XSpan outingSpan(const ComponentBuildInputs& in) {
    if (in.slots.empty()) {
        return {0.0, 1.0};
    }
    const XSpan ext = worksheetMainMapper(in).outingExtent();
    const double len = ext.length();
    return {0.0, len > 0.0 ? len : 1.0};
}

// ---- layout

constexpr float kTopMarginLogical = 4.0F;
constexpr float kRightMarginLogical = 4.0F;

struct OutingLayout {
    float dpr = 1.0F;
    RectPx bounds;
    RectPx caption;
    RectPx band; // the range band over the trace area
    RectPx keyColumn;
    RectPx traceArea;
    RectPx xAxis;
    int traceWidthPx = 0;
    std::vector<TdGroupLayout> groups;
    XSpan span;
};

OutingLayout layoutOf(const ComponentBuildInputs& in, const OutingBody& outing) {
    const TimeDistanceBody& body = outing.graph;
    OutingLayout lay;
    const float dpr = geom::dprOf(in);
    lay.dpr = dpr;
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    lay.bounds = RectPx{0.0F, 0.0F, w, h};
    lay.span = outingSpan(in);
    const project::Scheme& scheme = geom::schemeOf(in);
    const ITextMetrics& metrics = geom::metricsOf(in);
    lay.caption = geom::captionRect(in);
    const float top = lay.caption.h;
    const float xAxisH = body.showXAxis ? geom::snap(geom::kXAxisLogical * dpr) : 0.0F;
    const float axisW = body.showYAxis ? geom::snap(geom::kAxisStripLogical * dpr) : 0.0F;
    const float bandH = geom::snap(BandLayout::kBandHeight * dpr);
    const float topMargin = geom::snap(kTopMarginLogical * dpr);
    const float rightMargin = geom::snap(kRightMarginLogical * dpr);
    const std::vector<std::size_t> slots = drawnSlots(in, outing);

    const bool showKey = body.keyMode != "none";
    const std::vector<FlatTrace> flat = flatTraces(body);
    std::vector<KeyRow> allRows;
    std::vector<std::vector<KeyRow>> groupRows(body.groups.size());
    std::vector<std::vector<int>> groupTraces(body.groups.size());
    for (const FlatTrace& ft : flat) {
        if (!ft.trace->visible) {
            continue;
        }
        const auto index = static_cast<std::size_t>(ft.index);
        const ResolvedChannel* mainRc =
            in.slots.empty() ? nullptr : geom::channelOf(in.slots.front(), index, ft.trace->channel);
        KeyRow row;
        row.label = labelOf(*ft.trace, mainRc);
        row.missing = mainRc == nullptr || !mainRc->descriptor;
        row.group = ft.group;
        row.trace = ft.index;
        row.active = ft.trace->channel == in.spec.activeChannel;
        const Slot mainSlot = in.slots.empty() ? Slot{} : in.slots.front().slot;
        row.colour = traceColour(scheme, in.selection, mainSlot, colourIndexOf(mainRc, ft.index));
        if (body.keyValues) {
            const std::string tmpl = valueTemplate(dpsOf(*ft.trace, mainRc));
            for (std::size_t s = 0; s < std::max<std::size_t>(1, slots.size()); ++s) {
                const std::size_t k = s < slots.size() ? slots[s] : 0;
                const ResolvedChannel* rc =
                    k < in.slots.size() ? geom::channelOf(in.slots[k], index, ft.trace->channel) : nullptr;
                const Slot slot = k < in.slots.size() ? in.slots[k].slot : Slot{};
                row.slotValues.push_back(
                    KeyValue{tmpl, traceColour(scheme, in.selection, slot, colourIndexOf(rc, ft.index))});
            }
        }
        allRows.push_back(row);
        groupRows[static_cast<std::size_t>(ft.group)].push_back(row);
        groupTraces[static_cast<std::size_t>(ft.group)].push_back(ft.index);
    }
    const float rowH = metrics.lineHeightPx(TextStyleId::Key, dpr);
    KeyGeometry all;
    float keyW = 0.0F;
    if (showKey) {
        all = KeyLayout::layout(allRows, metrics, w, rowH, dpr);
        keyW = geom::snap(all.widthPx);
    }
    const bool keyRight = body.keyPos == "right";
    const float plotBottom = std::max(top, h - xAxisH);
    lay.keyColumn = keyW > 0.0F ? RectPx{keyRight ? w - keyW : 0.0F, top, keyW, plotBottom - top} : RectPx{};
    const float traceX = (keyRight ? 0.0F : keyW) + axisW;
    const float traceRight = keyRight ? w - keyW - rightMargin : w - rightMargin;
    const float traceW = std::max(0.0F, traceRight - traceX);
    const float bandTop = std::min(plotBottom, top);
    lay.band = RectPx{traceX, bandTop, traceW, std::min(bandH, plotBottom - bandTop)};
    const float traceTop = std::min(plotBottom, bandTop + lay.band.h + topMargin);
    lay.traceArea = RectPx{traceX, traceTop, traceW, plotBottom - traceTop};
    lay.traceWidthPx = static_cast<int>(lay.traceArea.w);
    lay.xAxis = xAxisH > 0.0F ? RectPx{traceX, plotBottom, lay.traceArea.w, xAxisH} : RectPx{};

    const std::size_t n = body.groups.size();
    const bool allDefault = std::all_of(body.groups.begin(), body.groups.end(), [](const TdGroup& g) {
        return g.edgeTop == 0.0 && g.edgeBottom == 1.0;
    });
    for (std::size_t g = 0; g < n; ++g) {
        double e0 = std::clamp(body.groups[g].edgeTop, 0.0, 1.0);
        double e1 = std::clamp(body.groups[g].edgeBottom, 0.0, 1.0);
        if (allDefault && n > 1) {
            e0 = static_cast<double>(g) / static_cast<double>(n);
            e1 = static_cast<double>(g + 1) / static_cast<double>(n);
        }
        if (e1 < e0) {
            std::swap(e0, e1);
        }
        TdGroupLayout gl;
        gl.group = static_cast<int>(g);
        const float y0 = lay.traceArea.y + geom::snap(static_cast<float>(e0) * lay.traceArea.h);
        const float y1 = lay.traceArea.y + geom::snap(static_cast<float>(e1) * lay.traceArea.h);
        gl.panel = RectPx{lay.traceArea.x, y0, lay.traceArea.w, y1 - y0};
        gl.axisStrip = RectPx{traceX - axisW, y0, axisW, y1 - y0};
        const float keyY = g == 0 ? top : y0;
        gl.key = RectPx{lay.keyColumn.x, keyY, keyW, y1 - keyY};
        gl.rows = std::move(groupRows[g]);
        gl.traces = std::move(groupTraces[g]);
        if (showKey) {
            gl.keyGeometry = KeyLayout::layout(gl.rows, metrics, w, rowH, dpr);
            gl.keyGeometry.widthPx = keyW;
            gl.keyGeometry.labelWidthPx = all.labelWidthPx + (keyW - all.widthPx);
            gl.keyGeometry.valueWidthPx = all.valueWidthPx;
            for (RectPx& r : gl.keyGeometry.rowRects) {
                r.w = keyW;
                r.y += geom::snap(2.0F * dpr);
            }
        }
        lay.groups.push_back(std::move(gl));
    }
    return lay;
}

// ---- the range band: laps of the Main outing

struct Lap {
    Micros start = 0;
    Micros end = 0;
    std::string name;
};

std::vector<Lap> lapsOf(const ComponentBuildInputs& in) {
    std::vector<Lap> laps;
    if (in.slots.empty()) {
        return laps;
    }
    const SlotData& main = in.slots.front();
    const model::DataLayer* layer = main.source ? main.source->layer(main.layer) : nullptr;
    const model::RangeGroup* group = layer != nullptr ? layer->ranges.find("Outings:Laps") : nullptr;
    if (group != nullptr) {
        for (const model::Range& r : group->ranges) {
            if (r.end > main.outing.start && r.start < main.outing.end) {
                laps.push_back(Lap{r.start, r.end, r.name});
            }
        }
    }
    if (laps.empty() && main.chain != nullptr) {
        for (const LapPiece& p : main.chain->pieces()) {
            laps.push_back(
                Lap{p.start, p.end, p.start == main.range.start ? main.slot.range.name : std::string{}});
        }
    }
    if (laps.empty()) {
        laps.push_back(Lap{main.range.start, main.range.end, main.slot.range.name});
    }
    return laps;
}

BandColours bandColoursOf(const geom::Colours& c) {
    BandColours b;
    b.text = c.foreground;
    b.background = c.keyBackground;
    b.frame = c.frame;
    b.sectionCorner = c.sectionCorner;
    b.sectionStraight = c.sectionStraight;
    b.marker = c.marker;
    b.annotation = c.annotation;
    b.deviceEvent = c.deviceEvent;
    return b;
}

// ---- scales

struct GroupScale {
    geom::Scale scale;
    const ResolvedChannel* axis = nullptr; // the first readable trace of the group in the Main slot
};

std::optional<double> manualIn(double v, const std::string& unit, const ResolvedChannel& axis,
                               const units::UnitRegistry* registry) {
    return geom::unitValueIn(analysis::UnitValue{v, unit}, axis.displayUnit, &axis, registry);
}

} // namespace

// ---- OutingBody

Result<OutingBody> OutingBody::parse(const JsonDoc& body) {
    Result<TimeDistanceBody> graph = TimeDistanceBody::parse(body);
    if (!graph) {
        return Failure{graph.error()};
    }
    OutingBody out;
    out.graph = std::move(*graph);
    if (body.isObject()) {
        const json j = jsonio::fromDoc(body);
        if (const auto it = j.find("enableOverlays"); it != j.end() && !it->is_null()) {
            if (!it->is_boolean()) {
                ErrorContext ctx;
                ctx.detail = "/enableOverlays";
                return fail(ErrorCode::Invalid, "outing body: expected a boolean at /enableOverlays", ctx);
            }
            out.enableOverlays = it->get<bool>();
        }
    }
    return out;
}

JsonDoc OutingBody::toJson() const {
    const JsonDoc base = graph.toJson();
    json j = detail::mergeOver(base);
    detail::put(j, "enableOverlays", enableOverlays, false);
    return detail::docOf(j, base);
}

std::vector<std::string> OutingBody::channelNames() const {
    return graph.channelNames();
}

// ---- Static

Result<ComponentBuild> OutingGeometry::buildStatic(const ComponentBuildInputs& inputs,
                                                   const CancelToken& cancel) {
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    DrawList& list = build.list;
    list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const project::Scheme& scheme = geom::schemeOf(inputs);
    const ITextMetrics& metrics = geom::metricsOf(inputs);
    const geom::Colours colours(scheme);
    const OutingBody outing = bodyOf(inputs);
    const TimeDistanceBody& body = outing.graph;
    const OutingLayout lay = layoutOf(inputs, outing);
    const float lineW = geom::lineWidthOf(dpr);
    list.clear(colours.background);
    if (lay.keyColumn.w > 0.0F) {
        list.filledRect(lay.keyColumn, colours.keyBackground);
    }
    if (inputs.slots.empty()) {
        return build;
    }
    const SlotData& mainSlot = inputs.slots.front();
    const auto traceX = static_cast<double>(lay.traceArea.x);
    const auto width = static_cast<double>(lay.traceWidthPx);

    // Range band: one cell per lap, the Main lap tinted.
    if (lay.band.h > 0.0F && lay.band.w > 0.0F) {
        BandLayoutInput bands;
        bands.showSections = false;
        bands.showMarkers = false;
        const DomainMapper laps0 = outingMapper(inputs, 0, nullptr);
        const std::vector<Lap> laps = lapsOf(inputs);
        for (std::size_t i = 0; i < laps.size(); ++i) {
            const Lap& lap = laps[i];
            BandCell cell;
            cell.x0 = static_cast<float>(DomainMapper::pxOf(laps0.xOf(lap.start), lay.span, traceX, width) -
                                         traceX);
            cell.x1 =
                static_cast<float>(DomainMapper::pxOf(laps0.xOf(lap.end), lay.span, traceX, width) - traceX);
            cell.label = lap.name;
            cell.colour = lap.start == mainSlot.range.start ? colours.grid : 0;
            cell.slot = 0;
            cell.index = static_cast<int>(i);
            bands.ranges.push_back(std::move(cell));
        }
        const BandGeometry bg = BandLayout::layout(bands, lay.band, dpr);
        BandLayout::emit(list, bands, bg, metrics, bandColoursOf(colours), dpr, &build.hits);
    }

    // Traces from the summary path, per drawn slot.
    const TraceStyle style = body.traceStyle == "points" ? TraceStyle::Points : TraceStyle::Line;
    const std::vector<std::size_t> slots = drawnSlots(inputs, outing);
    const std::vector<std::string> names = body.channelNames();
    std::vector<GroupScale> scales(body.groups.size());
    struct Drawn {
        int group = 0;
        std::size_t slot = 0;
        bool active = false;
        TraceSpec spec;
        DecimatedTrace trace;
    };
    std::vector<Drawn> drawn;
    for (const TdGroupLayout& gl : lay.groups) {
        for (const int flatIndex : gl.traces) {
            const auto fi = static_cast<std::size_t>(flatIndex);
            const std::string& channel = names[fi];
            for (const std::size_t k : slots) {
                const ResolvedChannel* rc = geom::channelOf(inputs.slots[k], fi, channel);
                if (!geom::readable(rc)) {
                    continue;
                }
                GroupScale& gs = scales[static_cast<std::size_t>(gl.group)];
                if (gs.axis == nullptr) {
                    gs.axis = rc;
                }
                const DomainMapper map = outingMapper(inputs, k, &*rc->series);
                if (inputs.view.domain == Domain::Distance && !map.hasDistance()) {
                    continue;
                }
                Drawn d;
                d.group = gl.group;
                d.slot = k;
                d.active = channel == inputs.spec.activeChannel;
                d.spec.column = store::seriesValues(*rc->series);
                if (const auto* u = std::get_if<store::UniformSeries>(&*rc->series)) {
                    d.spec.rate = u->rate;
                    d.spec.t0 = u->t0;
                } else if (const auto* ts = std::get_if<store::TimestampedSeries>(&*rc->series)) {
                    d.spec.timesUs = ts->timesUs;
                }
                d.spec.kind = rc->descriptor->kind;
                d.spec.interp = rc->descriptor->interp;
                d.spec.toDisplay = gs.axis->toDisplay;
                d.spec.style = style;
                d.spec.colour =
                    traceColour(scheme, inputs.selection, inputs.slots[k].slot, colourIndexOf(rc, flatIndex));
                d.spec.widthLogicalPx = body.lineWidth * traceWidthScale(scheme, inputs.slots[k].slot);
                d.spec.dash = traceDash(scheme, inputs.slots[k].slot);
                d.trace.columns.resize(static_cast<std::size_t>(std::max(lay.traceWidthPx, 0)));
                Result<double> origin = TraceBuilder::decimateSummary(d.spec, map, lay.span, lay.traceWidthPx,
                                                                      d.trace.columns, cancel);
                if (!origin) {
                    if (origin.error().code == ErrorCode::Cancelled) {
                        return Failure{origin.error()};
                    }
                    continue;
                }
                d.trace.origin = *origin;
                drawn.push_back(std::move(d));
            }
        }
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "outing build cancelled");
    }

    // Scales (04 section 8.3, as time_distance): manual, else the Main source's whole-column summary, else
    // the drawn columns.
    for (const TdGroupLayout& gl : lay.groups) {
        const TdGroup& group = body.groups[static_cast<std::size_t>(gl.group)];
        GroupScale& gs = scales[static_cast<std::size_t>(gl.group)];
        if (gs.axis == nullptr) {
            continue;
        }
        if (group.scaleMode == ScaleMode::Manual && group.scaleMin && group.scaleMax) {
            const auto lo = manualIn(*group.scaleMin, group.scaleUnit, *gs.axis, inputs.units.get());
            const auto hi = manualIn(*group.scaleMax, group.scaleUnit, *gs.axis, inputs.units.get());
            if (lo && hi) {
                gs.scale.unite(*lo, *hi);
            }
        }
        bool fallback = false;
        const bool manualSet = gs.scale.valid();
        for (std::size_t r = 0; r < gl.traces.size() && !manualSet && !fallback; ++r) {
            const auto fi = static_cast<std::size_t>(gl.traces[r]);
            const ResolvedChannel* rc = geom::channelOf(mainSlot, fi, names[fi]);
            if (!geom::readable(rc)) {
                continue;
            }
            const geom::SummaryRange sr = geom::summaryRange(*rc, gs.axis->toDisplay);
            if (!sr.covered) {
                fallback = true;
                break;
            }
            if (sr.range.valid()) {
                gs.scale.unite(sr.range.vMin, sr.range.vMax);
            }
        }
        if (!gs.scale.valid() || fallback) {
            if (fallback) {
                gs.scale = geom::Scale{};
            }
            for (const Drawn& d : drawn) {
                if (d.group != gl.group) {
                    continue;
                }
                for (const M4Column& col : d.trace.columns) {
                    if ((col.flags & M4Flag::Empty) == 0) {
                        gs.scale.unite(d.trace.origin + static_cast<double>(col.lo),
                                       d.trace.origin + static_cast<double>(col.hi));
                    }
                }
            }
        }
        gs.scale.widenDegenerate();
    }

    // x axis.
    AxisSpec xSpec;
    xSpec.kind = inputs.view.domain == Domain::Time ? AxisKind::Time : AxisKind::Distance;
    xSpec.lo = lay.span.a;
    xSpec.hi = lay.span.b;
    xSpec.lengthPx = static_cast<float>(lay.traceWidthPx);
    xSpec.unit = inputs.view.domain == Domain::Distance ? "m" : "";
    const std::vector<AxisTick> xTicks = AxisLayout::ticks(xSpec, metrics, dpr);
    const float axisLineH = metrics.lineHeightPx(TextStyleId::Axis, dpr);

    for (std::size_t gi = 0; gi < lay.groups.size(); ++gi) {
        const TdGroupLayout& gl = lay.groups[gi];
        const TdGroup& group = body.groups[static_cast<std::size_t>(gl.group)];
        const geom::Scale& s = scales[static_cast<std::size_t>(gl.group)].scale;
        const RectPx& panel = gl.panel;
        if (panel.h <= 0.0F || panel.w <= 0.0F) {
            continue;
        }
        YMap ymap;
        ymap.yTopPx = panel.y;
        ymap.yBottomPx = panel.y + panel.h;
        ymap.vMin = s.valid() ? s.vMin : 0.0;
        ymap.vMax = s.valid() ? s.vMax : 1.0;
        std::vector<AxisTick> yGrid;
        std::vector<AxisTick> yTicks;
        if (s.valid()) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.lo = s.vMax;
            ySpec.hi = s.vMin;
            ySpec.lengthPx = panel.h;
            ySpec.vertical = true;
            yGrid = AxisLayout::ticks(ySpec, metrics, dpr);
            yTicks = geom::keepVertical(yGrid, axisLineH, 0.0F, panel.h);
        }
        if (body.showGrid) {
            std::vector<float> grid;
            for (const AxisTick& t : xTicks) {
                const float x = std::round(panel.x + t.posPx) + 0.5F;
                grid.insert(grid.end(), {x, panel.y, x, panel.y + panel.h});
            }
            for (const AxisTick& t : yGrid) {
                const float y = std::round(panel.y + t.posPx) + 0.5F;
                grid.insert(grid.end(), {panel.x, y, panel.x + panel.w, y});
            }
            if (!grid.empty()) {
                list.lines(grid.data(), grid.size() / 2, colours.grid, lineW);
            }
        }
        if (s.valid() && group.zeroLine && s.vMin < 0.0 && s.vMax > 0.0) {
            const float y = ymap.yOf(0.0);
            const std::array<float, 4> zero{panel.x, y, panel.x + panel.w, y};
            list.lines(zero.data(), 2, colours.foreground2, lineW);
        }
        list.scissor(panel);
        for (std::size_t k = inputs.slots.size(); k-- > 0;) { // overlays beneath Main
            for (int pass = 0; pass < 2; ++pass) {            // the active channel last within a slot
                for (const Drawn& d : drawn) {
                    if (d.group != gl.group || d.slot != k || d.active != (pass == 1)) {
                        continue;
                    }
                    TraceBuilder::emit(list, d.trace.columns, d.trace.origin, d.spec, ymap, panel.x, dpr);
                }
            }
        }
        list.scissor(RectPx{});
        if (gi + 1 < lay.groups.size()) {
            const float y = panel.y + panel.h - 0.5F * lineW;
            const std::array<float, 4> sep{panel.x - (body.showYAxis ? gl.axisStrip.w : 0.0F), y,
                                           panel.x + panel.w, y};
            list.lines(sep.data(), 2, colours.frame, lineW);
        }
        if (body.showYAxis && !yTicks.empty()) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.vertical = true;
            AxisLayout::emit(list, ySpec, yTicks, gl.axisStrip, colours.foreground, dpr);
        }
        HitResult panelHit;
        panelHit.kind = HitKind::Panel;
        panelHit.group = gl.group;
        build.hits.addRect(panel, panelHit);
        if (body.showYAxis) {
            HitResult axisHit;
            axisHit.kind = HitKind::Axis;
            axisHit.group = gl.group;
            build.hits.addRect(gl.axisStrip, axisHit);
        }
    }
    if (lay.traceArea.w > 0.0F && lay.traceArea.h > 0.0F) {
        list.rectOutline(lay.traceArea, colours.frame, lineW);
    }
    if (lay.xAxis.h > 0.0F && !xTicks.empty()) {
        AxisSpec spec = xSpec;
        const float leftSlack =
            body.showYAxis ? lay.traceArea.x - (body.keyPos == "right" ? 0.0F : lay.keyColumn.w) : 0.0F;
        std::vector<AxisTick> kept =
            geom::keepHorizontal(xTicks, metrics, dpr, -leftSlack, lay.bounds.w - lay.traceArea.x);
        if (!kept.empty() && !spec.unit.empty()) {
            const AxisTick& last = kept.back();
            const float lastRight = last.posPx + 0.5F * metrics.advancePx(TextStyleId::Axis, last.label, dpr);
            if (lastRight + metrics.advancePx(TextStyleId::Axis, spec.unit, dpr) + 2.0F * dpr > lay.xAxis.w) {
                spec.unit.clear();
            }
        }
        AxisLayout::emit(list, spec, kept, lay.xAxis, colours.foreground, dpr);
        HitResult axisHit;
        axisHit.kind = HitKind::Axis;
        build.hits.addRect(lay.xAxis, axisHit);
    }
    for (const TdGroupLayout& gl : lay.groups) {
        if (gl.key.w <= 0.0F) {
            continue;
        }
        KeyLayout::emitLabels(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground2);
        for (std::size_t r = 0; r < gl.rows.size() && r < gl.keyGeometry.rowRects.size(); ++r) {
            RectPx rect = gl.keyGeometry.rowRects[r];
            rect.x += gl.key.x;
            rect.y += gl.key.y;
            HitResult h;
            h.kind = HitKind::KeyLabel;
            h.group = gl.group;
            h.trace = gl.traces[r];
            build.hits.addRect(rect, h);
        }
    }
    return build;
}

// ---- Dynamic

RectPx OutingGeometry::zoomRect(const ComponentBuildInputs& inputs) {
    if (inputs.slots.empty()) {
        return {};
    }
    const OutingBody outing = bodyOf(inputs);
    const OutingLayout lay = layoutOf(inputs, outing);
    if (!(lay.traceArea.w > 0.0F) || !(lay.traceArea.h > 0.0F)) {
        return {};
    }
    if (inputs.view.domain == Domain::Distance && !geom::slotHasDistance(inputs.slots.front())) {
        return {};
    }
    const double origin = outingOrigin(inputs);
    const auto traceX = static_cast<double>(lay.traceArea.x);
    const auto width = static_cast<double>(lay.traceWidthPx);
    double x0 = DomainMapper::pxOf(inputs.view.zoom.a - origin, lay.span, traceX, width);
    double x1 = DomainMapper::pxOf(inputs.view.zoom.b - origin, lay.span, traceX, width);
    if (x1 < x0) {
        std::swap(x0, x1);
    }
    x0 = std::max(x0, traceX);
    x1 = std::min(x1, traceX + width);
    if (!(x1 > x0)) {
        return RectPx{static_cast<float>(traceX), lay.traceArea.y, 0.0F, lay.traceArea.h};
    }
    return RectPx{static_cast<float>(x0), lay.traceArea.y, static_cast<float>(x1 - x0), lay.traceArea.h};
}

DrawList OutingGeometry::buildDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    if (inputs.slots.empty()) {
        return list;
    }
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const OutingBody outing = bodyOf(inputs);
    const TimeDistanceBody& body = outing.graph;
    OutingLayout lay = layoutOf(inputs, outing);
    const float lineW = geom::lineWidthOf(dpr);
    const SlotData& mainSlot = inputs.slots.front();
    const bool haveX = inputs.view.domain == Domain::Time || geom::slotHasDistance(mainSlot);

    // The zoom rectangle across all panels, then the cursor line through it.
    const RectPx zoom = zoomRect(inputs);
    if (zoom.w > 0.0F) {
        list.filledRect(zoom, geom::withAlpha(colours.zoom, 0x40));
        list.rectOutline(zoom, geom::withAlpha(colours.zoom, 0xC0), lineW);
    }
    const double origin = haveX ? outingOrigin(inputs) : 0.0;
    const DomainMapper mainMap = worksheetMainMapper(inputs);
    if (haveX) {
        const double x = mainMap.xOf(mainSlot.range.start + inputs.view.cursorT) - origin;
        const double px = DomainMapper::pxOf(x, lay.span, static_cast<double>(lay.traceArea.x),
                                             static_cast<double>(lay.traceWidthPx));
        if (px >= static_cast<double>(lay.traceArea.x) &&
            px <= static_cast<double>(lay.traceArea.x + lay.traceArea.w)) {
            const float sx = std::round(static_cast<float>(px)) + 0.5F;
            std::vector<float> segs;
            for (const TdGroupLayout& gl : lay.groups) {
                segs.insert(segs.end(), {sx, gl.panel.y, sx, gl.panel.y + gl.panel.h});
            }
            if (!segs.empty()) {
                list.lines(segs.data(), segs.size() / 2, colours.cursor, lineW);
            }
        }
    }

    // Key values at the cursor per drawn slot (overlays at k.start + cursorT: aligned at lap start).
    if (!body.keyValues || lay.keyColumn.w <= 0.0F) {
        return list;
    }
    const std::vector<std::size_t> slots = drawnSlots(inputs, outing);
    const std::vector<FlatTrace> flat = flatTraces(body);
    for (TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.rows.size(); ++r) {
            const auto fi = static_cast<std::size_t>(gl.traces[r]);
            const TdTrace* trace = flat[fi].trace;
            KeyRow& row = gl.rows[r];
            for (std::size_t s = 0; s < row.slotValues.size(); ++s) {
                std::string text;
                if (s < slots.size() && haveX) {
                    const SlotData& slot = inputs.slots[slots[s]];
                    const ResolvedChannel* rc = geom::channelOf(slot, fi, trace->channel);
                    if (rc != nullptr && rc->descriptor) {
                        if (rc->status == model::ChannelStatus::Pending || !rc->series) {
                            text = geom::kPendingText;
                        } else {
                            const Micros t = slot.range.start + inputs.view.cursorT;
                            const store::ValueSample v =
                                store::Resampler::sampleAt(*rc->series, t, rc->descriptor->interp);
                            if (v.state == store::SampleState::Absent) {
                                text = geom::kPendingText;
                            } else if (v.state == store::SampleState::Valid) {
                                text = geom::formatValue(rc->toDisplay(v.value), dpsOf(*trace, rc));
                            }
                        }
                    } else if (s > 0) {
                        text = "(not in " + slot.slot.range.name + ")";
                    }
                }
                row.slotValues[s].text = std::move(text);
            }
        }
        KeyLayout::emitValues(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground,
                              colours.keyBackground);
    }
    return list;
}

// ---- Chrome

DrawList OutingGeometry::buildChrome(const ComponentBuildInputs& inputs, bool pending) {
    DrawList list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const OutingBody outing = bodyOf(inputs);
    const TimeDistanceBody& body = outing.graph;
    const OutingLayout lay = layoutOf(inputs, outing);
    std::string autoText;
    for (const TdGroup& g : body.groups) {
        for (const TdTrace& t : g.traces) {
            if (t.visible) {
                autoText += (autoText.empty() ? "" : ", ") + t.channel;
            }
        }
    }
    geom::emitCaption(list, lay.caption, geom::captionText(inputs, autoText), colours, dpr);
    bool activeDone = false;
    for (const TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.rows.size() && !activeDone && gl.key.w > 0.0F; ++r) {
            if (!gl.rows[r].active) {
                continue;
            }
            RectPx sq = gl.keyGeometry.squareRect(r);
            sq.x += gl.key.x;
            sq.y += gl.key.y;
            list.filledRect(sq, gl.rows[r].colour);
            activeDone = true;
        }
    }
    if (pending || geom::anyPending(inputs)) {
        geom::emitPendingStripe(list, lay.traceArea, colours, dpr);
    }
    if (inputs.view.domain == Domain::Distance && !inputs.slots.empty() &&
        !geom::slotHasDistance(inputs.slots.front())) {
        const std::string name = sourceName(inputs.slots.front());
        geom::emitBanner(list, lay.traceArea, lay.traceArea.y + 4.0F * dpr,
                         name.empty() ? std::string("No distance data") : "No distance data for " + name,
                         colours, dpr);
    }
    geom::emitFocus(list, inputs, colours, dpr);
    return list;
}

} // namespace esses::plot
