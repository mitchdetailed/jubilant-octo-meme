// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackPositionBody and TrackGeometry (04 section 14). The map (ComponentBuildInputs::trackMap) is drawn in
// the component below the caption, inset by 6 logical px, fitted by TrackGeometry::fit: the bounding box of
// the rotated centre/inner/outer lines (map rotation + body rotation, counter-clockwise, ENU y up) centred in
// the area at the largest scale that fits; scale.mode manual multiplies that scale by `zoom` and shifts the
// map by (panX, panY) metres (right, up), so a resize keeps the framing. Sizes (logical px): `cursor` is the
// cursor dot diameter, `detail` the centreline width, `material` the extra width of the zoom span on each
// side. Positions along the track interpolate the centreline's cumulative distance (a closed map wraps
// through its first point at lengthM). The lap's track distance follows the origin rule of 04 section 14 item
// 5: a lap that starts at a BCN measures forward, any other backwards from its end beacon (trackLength -
// (lapLength - d)); a layer without BCN markers counts as starting at a beacon.

#include "body_json.hpp"
#include "geometry_common.hpp"

#include <esses/analysis/track_sections_rule.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/track_geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace esses::plot {

namespace {

using nlohmann::json;

constexpr float kInsetLogical = 6.0F;
constexpr float kMarkHalfLogical = 4.0F;
constexpr float kLabelOffsetLogical = 10.0F;
constexpr float kHitPadLogical = 4.0F;

TrackPositionBody bodyOf(const ComponentBuildInputs& in) {
    Result<TrackPositionBody> body = TrackPositionBody::parse(in.spec.body);
    return body ? std::move(*body) : TrackPositionBody{};
}

// ---- the centreline as a distance-parametrised polyline

struct Centreline {
    std::vector<analysis::TrackPoint> pts; // closed maps end with the first point again at lengthM
    double length = 0.0;
    bool closed = true;

    explicit Centreline(const analysis::TrackMap& map) : pts(map.centre), closed(map.closed) {
        length = map.lengthM > 0.0 ? map.lengthM : (pts.empty() ? 0.0 : pts.back().dist);
        if (closed && !pts.empty() && pts.back().dist < length) {
            analysis::TrackPoint first = pts.front();
            first.dist = length;
            pts.push_back(first);
        }
    }

    bool usable() const noexcept { return pts.size() >= 2 && length > 0.0; }

    // Distance wrapped (closed) or clamped (open) into [0, length].
    double normalise(double d) const noexcept {
        if (!std::isfinite(d)) {
            return 0.0;
        }
        if (!closed) {
            return std::clamp(d, 0.0, length);
        }
        double r = std::fmod(d, length);
        if (r < 0.0) {
            r += length;
        }
        return r;
    }

    analysis::TrackPoint at(double d) const noexcept {
        d = normalise(d);
        const auto it = std::upper_bound(pts.begin(), pts.end(), d,
                                         [](double v, const analysis::TrackPoint& p) { return v < p.dist; });
        if (it == pts.begin()) {
            return pts.front();
        }
        if (it == pts.end()) {
            return pts.back();
        }
        const analysis::TrackPoint& b = *it;
        const analysis::TrackPoint& a = *(it - 1);
        const double span = b.dist - a.dist;
        const double f = span > 0.0 ? (d - a.dist) / span : 0.0;
        return analysis::TrackPoint{a.x + f * (b.x - a.x), a.y + f * (b.y - a.y), d};
    }

    // The unit tangent at d (ENU), from the segment containing it.
    std::pair<double, double> tangent(double d) const noexcept {
        d = normalise(d);
        auto it = std::upper_bound(pts.begin(), pts.end(), d,
                                   [](double v, const analysis::TrackPoint& p) { return v < p.dist; });
        if (it == pts.begin()) {
            ++it;
        }
        if (it == pts.end()) {
            --it;
        }
        const double dx = it->x - (it - 1)->x;
        const double dy = it->y - (it - 1)->y;
        const double n = std::sqrt(dx * dx + dy * dy); // sqrt is correctly rounded everywhere, hypot is not
        return n > 0.0 ? std::pair{dx / n, dy / n} : std::pair{1.0, 0.0};
    }

    // The vertices from a to a + len (len >= 0, at most one lap), in ENU.
    std::vector<analysis::TrackPoint> span(double a, double len) const {
        std::vector<analysis::TrackPoint> out;
        if (!usable() || !(len > 0.0)) {
            return out;
        }
        len = std::min(len, length);
        const double start = normalise(a);
        double end = start + len;
        const auto appendRun = [&](double lo, double hi) {
            out.push_back(at(lo));
            for (const analysis::TrackPoint& p : pts) {
                if (p.dist > lo && p.dist < hi) {
                    out.push_back(p);
                }
            }
            out.push_back(at(hi)); // at(length) of a closed map is the start line
        };
        if (!closed) {
            end = std::min(end, length);
            appendRun(start, end);
            return out;
        }
        if (end <= length) {
            appendRun(start, end);
            return out;
        }
        appendRun(start, length);
        out.pop_back(); // the first point of the next run is the same position (the start line)
        appendRun(0.0, end - length);
        return out;
    }
};

std::vector<float> toPxPolyline(const std::vector<analysis::TrackPoint>& pts, const TrackFit& fit) {
    std::vector<float> xy;
    xy.reserve(pts.size() * 2);
    for (const analysis::TrackPoint& p : pts) {
        const PointPx q = fit.toPx(p.x, p.y);
        xy.push_back(q.x);
        xy.push_back(q.y);
    }
    return xy;
}

std::vector<float> toPxPolyline(const std::vector<analysis::TrackPoint>& pts, const TrackFit& fit,
                                bool closed) {
    std::vector<analysis::TrackPoint> all = pts;
    if (closed && all.size() >= 2) {
        all.push_back(all.front());
    }
    return toPxPolyline(all, fit);
}

RectPx mapArea(const ComponentBuildInputs& in) {
    const float dpr = geom::dprOf(in);
    const RectPx caption = geom::captionRect(in);
    const float inset = geom::snap(kInsetLogical * dpr);
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    const float top = caption.h + inset;
    return RectPx{inset, top, std::max(0.0F, w - 2.0F * inset), std::max(0.0F, h - top - inset)};
}

// The sections drawn: those of the first section set ("Default" when present).
std::vector<std::size_t> shownSections(const analysis::TrackMap& map) {
    std::vector<std::size_t> out;
    const std::vector<std::string> sets = map.sectionSets();
    if (sets.empty()) {
        return out;
    }
    for (std::size_t i = 0; i < map.sections.size(); ++i) {
        if (map.sections[i].set == sets.front()) {
            out.push_back(i);
        }
    }
    return out;
}

Rgba categoryColour(const analysis::TrackSection& s, const geom::Colours& c) {
    if (s.category == "corner") {
        return c.sectionCorner;
    }
    if (s.category == "straight") {
        return c.sectionStraight;
    }
    return c.foreground2;
}

// Whether the lap starting at `start` starts at a BCN (true when the layer has no BCN at all).
bool startsAtBeacon(const SlotData& slot, Micros start) {
    const model::DataLayer* layer = slot.source ? slot.source->layer(slot.layer) : nullptr;
    if (layer == nullptr) {
        return true;
    }
    bool anyBeacon = false;
    for (const model::Marker& m : layer->markers) {
        if (m.cls != model::MarkerClass::BCN || !m.enabled) {
            continue;
        }
        anyBeacon = true;
        if (m.t == start) {
            return true;
        }
    }
    return !anyBeacon;
}

// The worksheet Main mapper with alignment offsets excluded (the shared zoom does not fold them in).
DomainMapper mainMapper(const ComponentBuildInputs& in) {
    const SlotData& main = in.slots.front();
    RangeGeom range = main.range;
    range.offsetUs = 0;
    range.offsetM = 0.0;
    return {in.view.domain, range, main.outing, main.chain, Rate{}, 0};
}

// The layer time of the cursor in slot k (04 section 14 item 3).
std::optional<Micros> cursorTimeOf(const ComponentBuildInputs& in, std::size_t k) {
    const SlotData& slot = in.slots[k];
    if (in.view.domain == Domain::Time || k == 0) {
        return slot.range.start + in.view.cursorT;
    }
    const SlotData& main = in.slots.front();
    if (!geom::slotHasDistance(main) || !geom::slotHasDistance(slot)) {
        return std::nullopt;
    }
    const double x = mainMapper(in).xOf(main.range.start + in.view.cursorT);
    RangeGeom range = slot.range;
    range.offsetM = 0.0;
    const DomainMapper map{Domain::Distance, range, slot.outing, slot.chain, Rate{}, 0};
    return map.timeAt(x);
}

} // namespace

// ---- TrackPositionBody

Result<TrackPositionBody> TrackPositionBody::parse(const JsonDoc& body) {
    return jsonio::guardJson([&]() -> Result<TrackPositionBody> {
        TrackPositionBody out;
        out.rest = body;
        if (body.isNull()) {
            return out;
        }
        const json j = jsonio::fromDoc(body);
        if (!j.is_object()) {
            jsonio::violate("", "track_position body is not an object");
        }
        jsonio::ObjectReader r(j, "");
        if (const json* v = r.optional("showZoomSpan")) {
            out.showZoomSpan = jsonio::asBool(*v, r.child("showZoomSpan"));
        }
        if (const json* v = r.optional("showSectionLabels")) {
            out.showSectionLabels = jsonio::asBool(*v, r.child("showSectionLabels"));
        }
        if (const json* v = r.optional("showSectionMarks")) {
            out.showSectionMarks = jsonio::asBool(*v, r.child("showSectionMarks"));
        }
        if (const json* v = r.optional("showSectionColours")) {
            out.showSectionColours = jsonio::asBool(*v, r.child("showSectionColours"));
        }
        if (const json* sizes = r.optional("sizes")) {
            if (!sizes->is_object()) {
                jsonio::violate(r.child("sizes"), "expected an object");
            }
            jsonio::ObjectReader s(*sizes, r.child("sizes"));
            if (const json* v = s.optional("cursor")) {
                out.sizeCursor = static_cast<float>(jsonio::asNumber(*v, s.child("cursor")));
            }
            if (const json* v = s.optional("detail")) {
                out.sizeDetail = static_cast<float>(jsonio::asNumber(*v, s.child("detail")));
            }
            if (const json* v = s.optional("material")) {
                out.sizeMaterial = static_cast<float>(jsonio::asNumber(*v, s.child("material")));
            }
        }
        if (const json* scale = r.optional("scale")) {
            if (!scale->is_object()) {
                jsonio::violate(r.child("scale"), "expected an object");
            }
            jsonio::ObjectReader s(*scale, r.child("scale"));
            if (const json* v = s.optional("mode")) {
                out.scaleMode = jsonio::asString(*v, s.child("mode"));
                if (out.scaleMode != "fit" && out.scaleMode != "manual") {
                    jsonio::violate(s.child("mode"), R"(expected "fit" or "manual")");
                }
            }
            if (const json* v = s.optional("zoom")) {
                out.zoom = jsonio::asNumber(*v, s.child("zoom"));
            }
            if (const json* v = s.optional("panX")) {
                out.panX = jsonio::asNumber(*v, s.child("panX"));
            }
            if (const json* v = s.optional("panY")) {
                out.panY = jsonio::asNumber(*v, s.child("panY"));
            }
            if (const json* v = s.optional("rotation")) {
                out.rotationDeg = jsonio::asNumber(*v, s.child("rotation"));
            }
        }
        if (const json* v = r.optional("background")) {
            out.background = jsonio::asBool(*v, r.child("background"));
        }
        return out;
    });
}

JsonDoc TrackPositionBody::toJson() const {
    const TrackPositionBody d;
    json j = detail::mergeOver(rest);
    detail::put(j, "showZoomSpan", showZoomSpan, d.showZoomSpan);
    detail::put(j, "showSectionLabels", showSectionLabels, d.showSectionLabels);
    detail::put(j, "showSectionMarks", showSectionMarks, d.showSectionMarks);
    detail::put(j, "showSectionColours", showSectionColours, d.showSectionColours);
    json sizes = detail::childObject(j, "sizes");
    detail::put(sizes, "cursor", static_cast<double>(sizeCursor), static_cast<double>(d.sizeCursor));
    detail::put(sizes, "detail", static_cast<double>(sizeDetail), static_cast<double>(d.sizeDetail));
    detail::put(sizes, "material", static_cast<double>(sizeMaterial), static_cast<double>(d.sizeMaterial));
    detail::putObject(j, "sizes", std::move(sizes));
    json scale = detail::childObject(j, "scale");
    detail::put(scale, "mode", scaleMode, d.scaleMode);
    detail::put(scale, "zoom", zoom, d.zoom);
    detail::put(scale, "panX", panX, d.panX);
    detail::put(scale, "panY", panY, d.panY);
    detail::put(scale, "rotation", rotationDeg, d.rotationDeg);
    detail::putObject(j, "scale", std::move(scale));
    detail::put(j, "background", background, d.background);
    return detail::docOf(j, rest);
}

// ---- TrackFit and TrackGeometry

PointPx TrackFit::toPx(double xM, double yM) const noexcept {
    const double c = std::cos(rotationRad);
    const double s = std::sin(rotationRad);
    const double rx = xM * c - yM * s;
    const double ry = xM * s + yM * c;
    return PointPx{static_cast<float>(offsetX + scale * rx), static_cast<float>(offsetY - scale * ry)};
}

TrackFit TrackGeometry::fit(const analysis::TrackMap& map, RectPx area,
                            const TrackPositionBody& body) noexcept {
    TrackFit f;
    f.rotationRad = (map.rotationDeg + body.rotationDeg) * std::numbers::pi / 180.0;
    const double c = std::cos(f.rotationRad);
    const double s = std::sin(f.rotationRad);
    double x0 = std::numeric_limits<double>::infinity();
    double x1 = -x0;
    double y0 = x0;
    double y1 = -x0;
    for (const std::vector<analysis::TrackPoint>* line : {&map.centre, &map.inner, &map.outer}) {
        for (const analysis::TrackPoint& p : *line) {
            const double rx = p.x * c - p.y * s;
            const double ry = p.x * s + p.y * c;
            if (!std::isfinite(rx) || !std::isfinite(ry)) {
                continue;
            }
            x0 = std::min(x0, rx);
            x1 = std::max(x1, rx);
            y0 = std::min(y0, ry);
            y1 = std::max(y1, ry);
        }
    }
    const double ax = static_cast<double>(area.x) + 0.5 * static_cast<double>(area.w);
    const double ay = static_cast<double>(area.y) + 0.5 * static_cast<double>(area.h);
    if (!(x1 >= x0) || !(y1 >= y0)) {
        f.offsetX = ax;
        f.offsetY = ay;
        return f;
    }
    const double bw = x1 - x0;
    const double bh = y1 - y0;
    double fitScale = 1.0;
    if (bw > 0.0 && bh > 0.0) {
        fitScale = std::min(static_cast<double>(area.w) / bw, static_cast<double>(area.h) / bh);
    } else if (bw > 0.0) {
        fitScale = static_cast<double>(area.w) / bw;
    } else if (bh > 0.0) {
        fitScale = static_cast<double>(area.h) / bh;
    }
    if (!(fitScale > 0.0) || !std::isfinite(fitScale)) {
        fitScale = 1.0;
    }
    const bool manual = body.scaleMode == "manual";
    f.scale = manual && body.zoom > 0.0 ? fitScale * body.zoom : fitScale;
    const double panX = manual ? body.panX : 0.0;
    const double panY = manual ? body.panY : 0.0;
    const double cx = 0.5 * (x0 + x1);
    const double cy = 0.5 * (y0 + y1);
    f.offsetX = ax - f.scale * cx + f.scale * panX;
    f.offsetY = ay + f.scale * cy - f.scale * panY;
    return f;
}

std::optional<double> TrackGeometry::trackDistanceAt(const SlotData& slot, const analysis::TrackMap& map,
                                                     Micros t) {
    if (slot.chain == nullptr || !(map.lengthM > 0.0)) {
        return std::nullopt;
    }
    const std::vector<LapPiece>& pieces = slot.chain->pieces();
    if (pieces.empty() || pieces.back().end <= pieces.front().start) {
        return std::nullopt;
    }
    t = std::clamp(t, pieces.front().start, pieces.back().end - 1);
    const auto it = std::find_if(pieces.begin(), pieces.end(),
                                 [t](const LapPiece& p) { return t >= p.start && t < p.end; });
    if (it == pieces.end()) {
        return std::nullopt;
    }
    double d = 0.0;
    double len = 0.0;
    if (it->lookup) {
        d = it->lookup->distAt(t);
        len = it->lookup->lengthM();
    } else if (it->idx) {
        d = it->idx->distAt(t);
        len = it->idx->lengthM();
    } else {
        return std::nullopt;
    }
    const double stretch = it->stretch > 0.0 ? it->stretch : 1.0;
    d *= stretch;
    len *= stretch;
    // The origin rule of the sections (D-195 (d)): a complete lap within the tolerance maps proportionally,
    // so the dot of a lap a little longer than the map reaches the line at the beacon, not before it.
    const double trackM = analysis::trackDistanceOfLapDistance(
        d, len, map.lengthM, startsAtBeacon(slot, it->start), startsAtBeacon(slot, it->end));
    if (map.closed) {
        double r = std::fmod(trackM, map.lengthM);
        return r < 0.0 ? r + map.lengthM : r;
    }
    return std::clamp(trackM, 0.0, map.lengthM);
}

Result<ComponentBuild> TrackGeometry::buildStatic(const ComponentBuildInputs& inputs,
                                                  const CancelToken& cancel) {
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    DrawList& list = build.list;
    list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const TrackPositionBody body = bodyOf(inputs);
    list.clear(colours.background);
    const RectPx area = mapArea(inputs);
    if (area.w > 0.0F && area.h > 0.0F) {
        HitResult panel;
        panel.kind = HitKind::Panel;
        build.hits.addRect(area, panel);
    }
    if (!inputs.trackMap) {
        return build;
    }
    const analysis::TrackMap& map = *inputs.trackMap;
    const Centreline centre(map);
    const TrackFit fit = TrackGeometry::fit(map, area, body);
    const float lineW = geom::lineWidthOf(dpr);
    const float detailW = std::max(1.0F, body.sizeDetail * dpr);

    // Inner and outer lines.
    for (const std::vector<analysis::TrackPoint>* line : {&map.inner, &map.outer}) {
        if (line->size() >= 2) {
            const std::vector<float> xy = toPxPolyline(*line, fit, map.closed);
            list.polyline(xy.data(), xy.size() / 2, colours.frame, lineW);
        }
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "track_position build cancelled");
    }
    const std::vector<std::size_t> sections = shownSections(map);
    // Section colouring beneath the centreline (wider, by category).
    if (body.showSectionColours && centre.usable()) {
        const float sectionW = std::max(detailW * 2.0F, detailW + 2.0F * dpr);
        for (const std::size_t i : sections) {
            const analysis::TrackSection& s = map.sections[i];
            const std::vector<float> xy = toPxPolyline(centre.span(s.startM, s.lengthM()), fit);
            if (xy.size() >= 4) {
                list.polyline(xy.data(), xy.size() / 2, categoryColour(s, colours), sectionW);
            }
        }
    }
    // The centreline.
    if (map.centre.size() >= 2) {
        const std::vector<float> xy = toPxPolyline(centre.pts, fit);
        list.polyline(xy.data(), xy.size() / 2, colours.trackLine, detailW);
    }
    // Section marks (ticks across the line at each start) and labels (beside the mid-point).
    if (centre.usable() && (body.showSectionMarks || body.showSectionLabels)) {
        std::vector<float> marks;
        const float half = kMarkHalfLogical * dpr + 0.5F * detailW;
        for (const std::size_t i : sections) {
            const analysis::TrackSection& s = map.sections[i];
            if (body.showSectionMarks) {
                const analysis::TrackPoint p = centre.at(s.startM);
                const auto [tx, ty] = centre.tangent(s.startM);
                const PointPx a = fit.toPx(p.x, p.y);
                const PointPx b = fit.toPx(p.x + tx, p.y + ty);
                double nx = -(static_cast<double>(b.y) - static_cast<double>(a.y));
                double ny = static_cast<double>(b.x) - static_cast<double>(a.x);
                const double n = std::sqrt(nx * nx + ny * ny);
                if (n > 0.0) {
                    nx /= n;
                    ny /= n;
                    const auto ax = static_cast<double>(a.x);
                    const auto ay = static_cast<double>(a.y);
                    const auto h = static_cast<double>(half);
                    marks.insert(marks.end(),
                                 {static_cast<float>(ax - nx * h), static_cast<float>(ay - ny * h),
                                  static_cast<float>(ax + nx * h), static_cast<float>(ay + ny * h)});
                }
            }
            if (body.showSectionLabels && !s.name.empty()) {
                const double mid = s.startM + 0.5 * s.lengthM();
                const analysis::TrackPoint p = centre.at(mid);
                const auto [tx, ty] = centre.tangent(mid);
                const PointPx a = fit.toPx(p.x, p.y);
                const PointPx b = fit.toPx(p.x + tx, p.y + ty);
                double nx = -(static_cast<double>(b.y) - static_cast<double>(a.y));
                double ny = static_cast<double>(b.x) - static_cast<double>(a.x);
                const double n = std::sqrt(nx * nx + ny * ny);
                if (n > 0.0) {
                    nx /= n;
                    ny /= n;
                }
                const auto off = static_cast<double>(kLabelOffsetLogical * dpr);
                list.textRun(s.name, static_cast<float>(static_cast<double>(a.x) + nx * off),
                             static_cast<float>(static_cast<double>(a.y) + ny * off), TextStyleId::TrackLabel,
                             TextAnchor::Centre, colours.foreground);
            }
        }
        if (!marks.empty()) {
            list.lines(marks.data(), marks.size() / 2, colours.foreground, lineW);
        }
    }
    // Section hit regions: one padded box per segment of each section's polyline.
    if (centre.usable()) {
        const float pad = kHitPadLogical * dpr + 0.5F * detailW;
        for (const std::size_t i : sections) {
            const analysis::TrackSection& s = map.sections[i];
            const std::vector<float> xy = toPxPolyline(centre.span(s.startM, s.lengthM()), fit);
            HitResult h;
            h.kind = HitKind::Section;
            h.section = static_cast<int>(i);
            for (std::size_t v = 0; v + 3 < xy.size(); v += 2) {
                const float x0 = std::min(xy[v], xy[v + 2]);
                const float x1 = std::max(xy[v], xy[v + 2]);
                const float y0 = std::min(xy[v + 1], xy[v + 3]);
                const float y1 = std::max(xy[v + 1], xy[v + 3]);
                build.hits.addRect(RectPx{x0 - pad, y0 - pad, x1 - x0 + 2.0F * pad, y1 - y0 + 2.0F * pad}, h);
            }
        }
    }
    return build;
}

DrawList TrackGeometry::buildDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    if (!inputs.trackMap || inputs.slots.empty()) {
        return list;
    }
    const float dpr = geom::dprOf(inputs);
    const project::Scheme& scheme = geom::schemeOf(inputs);
    const geom::Colours colours(scheme);
    const TrackPositionBody body = bodyOf(inputs);
    const analysis::TrackMap& map = *inputs.trackMap;
    const Centreline centre(map);
    if (!centre.usable()) {
        return list;
    }
    const TrackFit fit = TrackGeometry::fit(map, mapArea(inputs), body);
    const SlotData& main = inputs.slots.front();

    // The zoom span between the track positions of zoom.a and zoom.b along the Main chain.
    if (body.showZoomSpan && geom::slotHasDistance(main)) {
        const DomainMapper map0 = mainMapper(inputs);
        const Micros ta = map0.timeAt(inputs.view.zoom.a);
        const Micros tb = map0.timeAt(inputs.view.zoom.b);
        const std::optional<double> da = trackDistanceAt(main, map, std::min(ta, tb));
        if (da) {
            const double len = std::abs(main.chain->distAt(tb) - main.chain->distAt(ta));
            const std::vector<float> xy = toPxPolyline(centre.span(*da, len), fit);
            if (xy.size() >= 4) {
                const float w = std::max(1.0F, (body.sizeDetail + 2.0F * body.sizeMaterial) * dpr);
                list.polyline(xy.data(), xy.size() / 2, colours.zoom, w);
            }
        }
    }
    // Cursor dots: overlays beneath, Main on top.
    const float size = std::max(1.0F, body.sizeCursor * dpr);
    for (std::size_t k = inputs.slots.size(); k-- > 0;) {
        const std::optional<Micros> t = cursorTimeOf(inputs, k);
        if (!t) {
            continue;
        }
        const std::optional<double> d = trackDistanceAt(inputs.slots[k], map, *t);
        if (!d) {
            continue;
        }
        const analysis::TrackPoint p = centre.at(*d);
        const PointPx q = fit.toPx(p.x, p.y);
        const std::array<float, 2> xy{q.x, q.y};
        const Rgba colour =
            k == 0 ? colours.cursor : rangeKeyColour(scheme, inputs.slots[k].slot); // 06 5.3 3
        list.points(xy.data(), 1, colour, size);
    }
    return list;
}

DrawList TrackGeometry::buildChrome(const ComponentBuildInputs& inputs, bool pending) {
    DrawList list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const RectPx caption = geom::captionRect(inputs);
    const std::string autoText = inputs.trackMap ? inputs.trackMap->name : std::string("Track Position");
    geom::emitCaption(list, caption, geom::captionText(inputs, autoText), colours, dpr);
    const RectPx area = mapArea(inputs);
    if (pending || geom::anyPending(inputs)) {
        geom::emitPendingStripe(
            list, RectPx{0.0F, caption.h, static_cast<float>(inputs.layout.widthPx), 0.0F}, colours, dpr);
    }
    if (!inputs.trackMap) {
        geom::emitBanner(list, area, area.y + 4.0F * dpr, "No track map (Tools \xE2\x86\x92 Track Editor)",
                         colours, dpr);
    } else if (!inputs.slots.empty() && !geom::slotHasDistance(inputs.slots.front())) {
        geom::emitBanner(list, area, area.y + 4.0F * dpr, "No distance data", colours, dpr);
    }
    geom::emitFocus(list, inputs, colours, dpr);
    return list;
}

} // namespace esses::plot
