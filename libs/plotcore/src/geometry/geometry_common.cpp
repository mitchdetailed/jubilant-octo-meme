// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "geometry_common.hpp"

#include "../text_format.hpp"

#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>

#include <utility>

namespace esses::plot::geom {

Colours::Colours(const project::Scheme& s)
    : background(s.role("cptBackground", 0xFFFFFFFFU)), keyBackground(s.role("keyBackground", 0xF4F4F4FFU)),
      foreground(s.role("cptForeground", 0x202020FFU)), foreground2(s.role("cptForeground2", 0x707070FFU)),
      frame(s.role("cptFrame", 0xA0A0A0FFU)), grid(s.role("dataGrid", 0xE6E6E6FFU)),
      cursor(s.role("cursorMain", 0xFF0000FFU)), datum(s.role("cursorDatum", 0xC09000FFU)),
      zoom(s.role("cursorZoom", 0x3080FFFFU)), captionBackground(s.role("cptCaptionBackground", 0xE8E8E8FFU)),
      captionForeground(s.role("cptCaptionForeground", 0x202020FFU)),
      focus(s.role("cptFocusBorder", 0x2060FFFFU)), pending(s.role("pendingHatch", 0xC8C8C8FFU)),
      sectionCorner(s.role("sectionCorner", 0xFFD0A0FFU)),
      sectionStraight(s.role("sectionStraight", 0xD0E0FFFFU)), marker(s.role("markerBand", 0x404040FFU)),
      annotation(s.role("annotationMark", 0x2060FFFFU)), deviceEvent(s.role("deviceEvent", 0xC04000FFU)),
      trackLine(s.role("trackLine", 0x606060FFU)) {}

const project::Scheme& schemeOf(const ComponentBuildInputs& in) {
    static const project::Scheme kEmpty;
    return in.scheme ? *in.scheme : kEmpty;
}

const ITextMetrics& metricsOf(const ComponentBuildInputs& in) {
    static const FixedTextMetrics kFallback;
    return in.metrics ? *in.metrics : kFallback;
}

float dprOf(const ComponentBuildInputs& in) {
    return in.layout.dpr > 0.0F ? in.layout.dpr : 1.0F;
}

DrawList newList(const ComponentBuildInputs& in) {
    DrawList list;
    list.widthPx = in.layout.widthPx;
    list.heightPx = in.layout.heightPx;
    list.dpr = dprOf(in);
    return list;
}

std::size_t quads(DrawList& list, const float* xy, std::size_t vertexCount, Rgba colour) {
    DrawCmd cmd;
    cmd.kind = CmdKind::Quads;
    cmd.vertexOffset = static_cast<std::uint32_t>(list.verts.size() / 2);
    cmd.vertexCount = static_cast<std::uint32_t>(vertexCount);
    cmd.colour = colour;
    if (vertexCount > 0) {
        list.verts.insert(list.verts.end(), xy, xy + 2 * vertexCount);
    }
    list.cmds.push_back(cmd);
    return list.cmds.size() - 1;
}

// ---- channels and slots

const ResolvedChannel* channelOf(const SlotData& slot, std::size_t index, std::string_view name) {
    if (index < slot.channels.size() && slot.channels[index].name == name) {
        return &slot.channels[index];
    }
    for (const ResolvedChannel& c : slot.channels) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

bool readable(const ResolvedChannel* rc) {
    return rc != nullptr && rc->descriptor && rc->series && rc->status != model::ChannelStatus::Pending;
}

DomainMapper mapperOf(const ComponentBuildInputs& in, const SlotData& slot, const store::Series* series) {
    Rate rate;
    Micros t0 = 0;
    if (series != nullptr) {
        if (const auto* u = std::get_if<store::UniformSeries>(series)) {
            rate = u->rate;
            t0 = u->t0;
        }
    }
    return {in.view.domain, slot.range, slot.outing, slot.chain, rate, t0};
}

bool slotHasDistance(const SlotData& slot) {
    return slot.chain != nullptr && slot.chain->anyIndexed();
}

// ---- scales

void Scale::unite(double lo, double hi) noexcept {
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        return;
    }
    if (lo > hi) {
        std::swap(lo, hi);
    }
    if (!valid()) {
        vMin = lo;
        vMax = hi;
        return;
    }
    vMin = std::min(vMin, lo);
    vMax = std::max(vMax, hi);
}

void Scale::widenDegenerate() noexcept {
    if (valid() && vMax == vMin) {
        vMin -= 1.0;
        vMax += 1.0;
    }
}

SummaryRange summaryRange(const ResolvedChannel& rc, const units::Converter& toDisplay) {
    SummaryRange out;
    if (!rc.series) {
        return out;
    }
    const std::shared_ptr<const store::SummaryPyramid> summary =
        store::seriesValues(*rc.series).summarySnapshot();
    if (summary == nullptr || summary->size() == 0) {
        return out;
    }
    bool covered = false;
    const store::MinMax mm = summary->minmax(0, summary->size() - 1, &covered);
    out.covered = covered;
    if (covered && !mm.isEmpty()) {
        out.range.unite(toDisplay(static_cast<double>(mm.lo)), toDisplay(static_cast<double>(mm.hi)));
    }
    return out;
}

std::optional<double> unitValueIn(const analysis::UnitValue& value, const std::string& toUnit,
                                  const ResolvedChannel* axis, const units::UnitRegistry* registry) {
    if (value.unit == toUnit) {
        return value.v;
    }
    if (value.unit.empty()) { // base units of the axis channel
        return axis != nullptr ? std::optional<double>(axis->toDisplay(value.v))
                               : std::optional<double>(value.v);
    }
    if (registry != nullptr) {
        const std::optional<units::UnitId> from = registry->find(value.unit);
        const std::optional<units::UnitId> to = registry->find(toUnit);
        if (from && to) {
            return registry->toBase(*to).inverse()(registry->toBase(*from)(value.v));
        }
        if (from && axis != nullptr) {
            return axis->toDisplay(registry->toBase(*from)(value.v));
        }
    }
    return std::nullopt;
}

// ---- bands

std::optional<std::size_t> bandIndexOf(double v, std::span<const analysis::Band> bands) {
    if (std::isnan(v)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const analysis::Band& b = bands[i];
        const bool in = b.lessMin ? v < b.hi : (b.greaterMax ? v >= b.lo : (v >= b.lo && v < b.hi));
        if (in) {
            return i;
        }
    }
    return std::nullopt;
}

// ---- spans

std::vector<analysis::SampleSpan> visibleSpans(const ComponentBuildInputs& in, const SlotData& slot) {
    std::vector<analysis::SampleSpan> out;
    if (in.view.domain == Domain::Distance && !slotHasDistance(slot)) {
        return out;
    }
    const DomainMapper map = mapperOf(in, slot, nullptr);
    Micros t0 = map.timeAt(in.view.zoom.a);
    Micros t1 = map.timeAt(in.view.zoom.b);
    if (t1 < t0) {
        std::swap(t0, t1);
    }
    if (slot.outing.end > slot.outing.start) {
        t0 = std::max(t0, slot.outing.start);
        t1 = std::min(t1, slot.outing.end);
    }
    if (t1 <= t0) {
        return out;
    }
    const model::DataLayer* layer = slot.source ? slot.source->layer(slot.layer) : nullptr;
    const model::RangeGroup* laps = layer != nullptr ? layer->ranges.find("Outings:Laps") : nullptr;
    Micros c = t0;
    if (laps != nullptr) {
        for (const model::Range& lap : laps->ranges) {
            if (lap.end <= c) {
                continue;
            }
            if (lap.start >= t1) {
                break;
            }
            if (lap.start > c) {
                out.push_back(analysis::SampleSpan{c, lap.start, true});
                c = lap.start;
            }
            const Micros e = std::min(t1, lap.end);
            out.push_back(analysis::SampleSpan{c, e, lap.trusted});
            c = e;
        }
    }
    if (c < t1) {
        out.push_back(analysis::SampleSpan{c, t1, true});
    }
    return out;
}

// ---- text

std::string formatValue(double v, int decimals) {
    return detail::formatFixed(v, decimals);
}

int decimalsForStep(double step) {
    step = std::abs(step);
    if (!(step > 0.0) || !std::isfinite(step)) {
        return 0;
    }
    double scaled = step;
    for (int d = 0; d < 6; ++d) {
        if (std::abs(scaled - std::round(scaled)) <= 1e-9 * std::max(1.0, scaled)) {
            return d;
        }
        scaled *= 10.0;
    }
    return 6;
}

// ---- axes

std::vector<AxisTick> keepHorizontal(std::vector<AxisTick> ticks, const ITextMetrics& metrics, float dpr,
                                     float minPx, float maxPx) {
    std::vector<AxisTick> out;
    for (AxisTick& t : ticks) {
        const float half = 0.5F * metrics.advancePx(TextStyleId::Axis, t.label, dpr);
        if (t.posPx - half >= minPx && t.posPx + half <= maxPx) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::vector<AxisTick> keepVertical(std::vector<AxisTick> ticks, float lineH, float minPx, float maxPx) {
    std::vector<AxisTick> out;
    for (AxisTick& t : ticks) {
        if (t.posPx - 0.5F * lineH >= minPx && t.posPx + 0.5F * lineH <= maxPx) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

// ---- Chrome

RectPx captionRect(const ComponentBuildInputs& in) {
    if (!in.spec.caption.show) {
        return {};
    }
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    return RectPx{0.0F, 0.0F, w, std::min(h, snap(kCaptionLogical * dprOf(in)))};
}

std::string captionText(const ComponentBuildInputs& in, const std::string& autoText) {
    return in.spec.caption.autoText ? autoText : in.spec.caption.text;
}

void emitCaption(DrawList& list, const RectPx& rect, const std::string& text, const Colours& colours,
                 float dpr) {
    if (!(rect.h > 0.0F)) {
        return;
    }
    list.filledRect(rect, colours.captionBackground);
    if (!text.empty()) {
        list.textRun(text, rect.x + 4.0F * dpr, rect.y + 0.5F * rect.h, TextStyleId::Caption,
                     TextAnchor::MiddleLeft, colours.captionForeground, std::max(1.0F, rect.w - 8.0F * dpr));
    }
}

bool anyPending(const ComponentBuildInputs& in) {
    for (const SlotData& slot : in.slots) {
        for (const ResolvedChannel& rc : slot.channels) {
            if (rc.descriptor && rc.status == model::ChannelStatus::Pending) {
                return true;
            }
        }
    }
    return false;
}

void emitPendingStripe(DrawList& list, const RectPx& area, const Colours& colours, float dpr) {
    if (area.w > 0.0F) {
        list.filledRect(RectPx{area.x, area.y, area.w, std::round(3.0F * dpr)}, colours.pending);
    }
}

void emitFocus(DrawList& list, const ComponentBuildInputs& in, const Colours& colours, float dpr) {
    if (in.focused && in.layout.widthPx > 0 && in.layout.heightPx > 0) {
        list.rectOutline(
            RectPx{0.0F, 0.0F, static_cast<float>(in.layout.widthPx), static_cast<float>(in.layout.heightPx)},
            colours.focus, std::round(2.0F * dpr));
    }
}

void emitBanner(DrawList& list, const RectPx& area, float y, const std::string& text, const Colours& colours,
                float dpr) {
    if (area.w > 0.0F && !text.empty()) {
        list.textRun(text, area.x + 0.5F * area.w, y, TextStyleId::Banner, TextAnchor::TopCentre,
                     colours.foreground, std::max(1.0F, area.w - 8.0F * dpr), 0, colours.keyBackground);
    }
}

} // namespace esses::plot::geom
