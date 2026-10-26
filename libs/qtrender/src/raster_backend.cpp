// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "glyph_bins.hpp"
#include "paint_internal.hpp"
#include "span_raster.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QGlyphRun>
#include <QList>
#include <QPen>
#include <QPoint>
#include <QPolygonF>
#include <QRawFont>
#include <QRect>
#include <QRegion>
#include <QStaticText>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace esses::qt {

namespace {

bool vertexRangeOk(const plot::DrawList& list, const plot::DrawCmd& cmd) {
    return (static_cast<std::size_t>(cmd.vertexOffset) + cmd.vertexCount) * 2 <= list.verts.size();
}

} // namespace

namespace detail {

QColor toQColor(plot::Rgba c) {
    return QColor(static_cast<int>(project::red(c)), static_cast<int>(project::green(c)),
                  static_cast<int>(project::blue(c)), static_cast<int>(project::alpha(c)));
}

const SchemeFonts& defaultSchemeFonts() {
    static const SchemeFonts fonts;
    return fonts;
}

const FontEntry& schemeFontEntry(const SchemeFonts& fonts, plot::TextStyleId style, bool rotated) {
    const auto slot = static_cast<std::uint32_t>(static_cast<unsigned>(style) * 2U + (rotated ? 1U : 0U));
    if (const FontEntry* e = findThreadFont(fonts.id(), slot)) {
        return *e;
    }
    return insertThreadFont(fonts.id(), slot, fonts.font(style, rotated));
}

const FontEntry& schemeFontEntry(const SchemeFonts& fonts, plot::TextStyleId style, bool rotated,
                                 float scale) {
    if (!(scale > 0.0F) || !std::isfinite(scale) || scale == 1.0F) {
        return schemeFontEntry(fonts, style, rotated);
    }
    // Slots of scaled fonts: the scale in thousandths above the style slot (< 256), never 0 in the high bits.
    const auto milli = static_cast<std::uint32_t>(
        std::clamp(std::lround(static_cast<double>(scale) * 1000.0), 1L, static_cast<long>(0xFFFFFF)));
    const auto slot =
        (milli << 8U) | static_cast<std::uint32_t>(static_cast<unsigned>(style) * 2U + (rotated ? 1U : 0U));
    if (const FontEntry* e = findThreadFont(fonts.id(), slot)) {
        return *e;
    }
    QFont font = fonts.font(style, rotated);
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(font.pointSizeF() * static_cast<double>(milli) / 1000.0);
    } else if (font.pixelSize() > 0) {
        const double px = static_cast<double>(font.pixelSize()) * static_cast<double>(milli) / 1000.0;
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
    }
    return insertThreadFont(fonts.id(), slot, font);
}

QTransform TextPlacement::frame() const {
    QTransform t = QTransform::fromTranslate(origin.x(), origin.y());
    if (rotated) {
        t.rotate(-90.0);
    }
    return t;
}

std::optional<TextPlacement> placeText(const plot::DrawList& list, const plot::TextRun& run,
                                       const FontEntry& font, qreal dpr) {
    if (static_cast<std::size_t>(run.byteOffset) + run.byteLen > list.text.size() || !std::isfinite(run.x) ||
        !std::isfinite(run.y)) {
        return std::nullopt;
    }
    TextPlacement tp;
    tp.text = QString::fromUtf8(list.text.data() + run.byteOffset, static_cast<qsizetype>(run.byteLen));
    if (run.maxWidthPx > 0.0F) {
        tp.text = font.metrics.elidedText(tp.text, Qt::ElideRight, static_cast<qreal>(run.maxWidthPx) / dpr);
    }
    const qreal w = font.metrics.horizontalAdvance(tp.text);
    const qreal h = font.metrics.height();
    qreal left = 0.0;
    qreal top = 0.0;
    switch (run.anchor) {
    case plot::TextAnchor::TopLeft:
    case plot::TextAnchor::MiddleLeft:
    case plot::TextAnchor::BottomLeft:
        left = 0.0;
        break;
    case plot::TextAnchor::TopCentre:
    case plot::TextAnchor::Centre:
    case plot::TextAnchor::BottomCentre:
        left = -w / 2.0;
        break;
    case plot::TextAnchor::TopRight:
    case plot::TextAnchor::MiddleRight:
    case plot::TextAnchor::BottomRight:
        left = -w;
        break;
    }
    switch (run.anchor) {
    case plot::TextAnchor::TopLeft:
    case plot::TextAnchor::TopCentre:
    case plot::TextAnchor::TopRight:
        top = 0.0;
        break;
    case plot::TextAnchor::MiddleLeft:
    case plot::TextAnchor::Centre:
    case plot::TextAnchor::MiddleRight:
        top = -h / 2.0;
        break;
    case plot::TextAnchor::BottomLeft:
    case plot::TextAnchor::BottomCentre:
    case plot::TextAnchor::BottomRight:
        top = -h;
        break;
    }
    tp.origin = QPointF(static_cast<qreal>(run.x) / dpr, static_cast<qreal>(run.y) / dpr);
    tp.box = QRectF(left, top, w, h);
    tp.rotated = run.quarterTurns == 1;
    return tp;
}

CommandBounds commandBounds(const plot::DrawList& list, const plot::DrawCmd& cmd, const SchemeFonts& fonts) {
    CommandBounds out;
    const qreal dpr = list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : 1.0;
    switch (cmd.kind) {
    case plot::CmdKind::Clear:
        out.wholeLayer = true;
        return out;
    case plot::CmdKind::Scissor:
        return out;
    case plot::CmdKind::FilledRect:
        if (cmd.rect.w > 0.0F && cmd.rect.h > 0.0F) {
            out.rect = QRectF(static_cast<qreal>(cmd.rect.x), static_cast<qreal>(cmd.rect.y),
                              static_cast<qreal>(cmd.rect.w), static_cast<qreal>(cmd.rect.h))
                           .adjusted(-1.0, -1.0, 1.0, 1.0);
        }
        return out;
    case plot::CmdKind::Polyline:
    case plot::CmdKind::Lines:
    case plot::CmdKind::Points:
    case plot::CmdKind::Quads: {
        if (!vertexRangeOk(list, cmd)) {
            return out;
        }
        qreal x0 = std::numeric_limits<qreal>::max();
        qreal y0 = x0;
        qreal x1 = -x0;
        qreal y1 = -x0;
        const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
        for (std::uint32_t i = 0; i < cmd.vertexCount; ++i) {
            const auto x = static_cast<qreal>(v[2 * i]);
            const auto y = static_cast<qreal>(v[2 * i + 1]);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                continue;
            }
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        if (x0 > x1) {
            return out;
        }
        qreal margin = 1.0;
        if (cmd.kind == plot::CmdKind::Points) {
            margin = static_cast<qreal>(std::max(cmd.pointSizePx, 1.0F)) + 2.0;
        } else if (cmd.kind != plot::CmdKind::Quads) {
            margin = static_cast<qreal>(std::max(cmd.widthPx, 1.0F)) + 2.0;
        }
        out.rect =
            QRectF(QPointF(x0, y0), QPointF(x1, y1)).adjusted(-margin, -margin, margin + 1.0, margin + 1.0);
        return out;
    }
    case plot::CmdKind::TextRun: {
        if (cmd.textIndex >= list.texts.size()) {
            return out;
        }
        const plot::TextRun& run = list.texts[cmd.textIndex];
        const auto tp = placeText(list, run, runFontEntry(fonts, run), dpr);
        if (!tp) {
            return out;
        }
        const QRectF logical = tp->frame().mapRect(tp->box);
        out.rect = QRectF(logical.x() * dpr, logical.y() * dpr, logical.width() * dpr, logical.height() * dpr)
                       .adjusted(-1.0, -1.0, 1.0, 1.0);
        return out;
    }
    }
    return out;
}

} // namespace detail

namespace {

using detail::toQColor;

constexpr double kCoordLimit = 1.0e6; // keeps the rasteriser's fixed-point arithmetic in range
constexpr std::size_t kSlice = 4096;  // 04 section 6.1 item 3

int snapToPixel(float v) { // the device pixel containing v
    return static_cast<int>(std::clamp(std::floor(static_cast<double>(v)), -kCoordLimit, kCoordLimit));
}

int roundEdge(float v) {
    return static_cast<int>(std::clamp(std::round(static_cast<double>(v)), -kCoordLimit, kCoordLimit));
}

qreal clampCoord(float v) {
    return std::clamp(static_cast<qreal>(v), -kCoordLimit, kCoordLimit);
}

QRect snapRect(const plot::RectPx& r) {
    const int x0 = roundEdge(r.x);
    const int y0 = roundEdge(r.y);
    const int x1 = roundEdge(r.x + r.w);
    const int y1 = roundEdge(r.y + r.h);
    return QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

bool finiteVertex(const float* v, std::size_t i) {
    return std::isfinite(v[2 * i]) && std::isfinite(v[2 * i + 1]);
}

Qt::PenStyle penStyle(plot::DashId dash) {
    switch (dash) {
    case plot::DashId::Solid:
        return Qt::SolidLine;
    case plot::DashId::Dash:
        return Qt::DashLine;
    case plot::DashId::Dot:
        return Qt::DotLine;
    case plot::DashId::DashDot:
        return Qt::DashDotLine;
    }
    return Qt::SolidLine;
}

// `square`: a stroke snapped to whole pixels (crispStroke below) joins its right angles with a mitre, which
// covers the corner's pixels exactly where a round join would leave them partly covered.
QPen strokePen(plot::Rgba colour, float widthPx, plot::DashId dash, bool aliased, bool square = false) {
    QPen pen(toQColor(colour));
    pen.setWidthF(aliased ? 1.0 : static_cast<qreal>(widthPx));
    pen.setStyle(penStyle(dash));
    pen.setCapStyle(aliased ? Qt::SquareCap : Qt::FlatCap);
    pen.setJoinStyle(square ? Qt::MiterJoin : Qt::RoundJoin);
    return pen;
}

struct Scratch {
    std::vector<QPoint> points;
    std::vector<QPointF> pointsF;
    std::vector<QPoint> single;
};

Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

// Draws vertices [begin, end) of `v` (all finite) as one polyline in slices of <= kSlice points.
void drawPolylineRun(QPainter& p, const float* v, std::size_t begin, std::size_t end, bool aliased) {
    if (end - begin < 2) {
        return;
    }
    Scratch& s = scratch();
    for (std::size_t first = begin; first + 1 < end; first += kSlice - 1) {
        const std::size_t last = std::min(first + kSlice, end);
        if (aliased) {
            // Consecutive vertices in one device pixel collapse (M4 emits up to four per column): fewer
            // segments, and no trailing zero-length segment that would drop the run's last pixel.
            s.points.clear();
            for (std::size_t i = first; i < last; ++i) {
                const QPoint q(snapToPixel(v[2 * i]), snapToPixel(v[2 * i + 1]));
                if (s.points.empty() || s.points.back() != q) {
                    s.points.push_back(q);
                }
            }
            if (s.points.size() == 1) {
                p.drawPoint(s.points.front());
            } else {
                p.drawPolyline(s.points.data(), static_cast<int>(s.points.size()));
            }
        } else {
            s.pointsF.clear();
            for (std::size_t i = first; i < last; ++i) {
                s.pointsF.emplace_back(clampCoord(v[2 * i]), clampCoord(v[2 * i + 1]));
            }
            p.drawPolyline(s.pointsF.data(), static_cast<int>(s.pointsF.size()));
        }
    }
}

void drawPolyline(QPainter& p, const plot::DrawList& list, const plot::DrawCmd& cmd, bool square = false) {
    const bool aliased = cmd.widthPx <= 1.0F;
    p.setRenderHint(QPainter::Antialiasing, !aliased);
    p.setPen(strokePen(cmd.colour, cmd.widthPx, cmd.dash, aliased, square));
    p.setBrush(Qt::NoBrush);
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    const std::size_t n = cmd.vertexCount;
    std::size_t i = 0;
    while (i < n) {
        while (i < n && !finiteVertex(v, i)) {
            ++i;
        }
        std::size_t j = i;
        while (j < n && finiteVertex(v, j)) {
            ++j;
        }
        drawPolylineRun(p, v, i, j, aliased);
        i = j;
    }
}

void drawLines(QPainter& p, const plot::DrawList& list, const plot::DrawCmd& cmd, bool square = false) {
    const bool aliased = cmd.widthPx <= 1.0F;
    p.setRenderHint(QPainter::Antialiasing, !aliased);
    p.setPen(strokePen(cmd.colour, cmd.widthPx, cmd.dash, aliased, square));
    p.setBrush(Qt::NoBrush);
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    Scratch& s = scratch();
    s.points.clear();
    s.pointsF.clear();
    s.single.clear();
    const auto flush = [&] {
        if (!s.points.empty()) {
            p.drawLines(s.points.data(), static_cast<int>(s.points.size() / 2));
        }
        if (!s.pointsF.empty()) {
            p.drawLines(s.pointsF.data(), static_cast<int>(s.pointsF.size() / 2));
        }
        if (!s.single.empty()) { // aliased segments inside one pixel: a zero-length line draws nothing
            p.drawPoints(s.single.data(), static_cast<int>(s.single.size()));
        }
        s.points.clear();
        s.pointsF.clear();
        s.single.clear();
    };
    for (std::size_t i = 0; i + 1 < cmd.vertexCount; i += 2) {
        if (!finiteVertex(v, i) || !finiteVertex(v, i + 1)) {
            continue;
        }
        if (aliased) {
            const QPoint a(snapToPixel(v[2 * i]), snapToPixel(v[2 * i + 1]));
            const QPoint b(snapToPixel(v[2 * i + 2]), snapToPixel(v[2 * i + 3]));
            if (a == b) {
                s.single.push_back(a);
            } else {
                s.points.push_back(a);
                s.points.push_back(b);
            }
        } else {
            s.pointsF.emplace_back(clampCoord(v[2 * i]), clampCoord(v[2 * i + 1]));
            s.pointsF.emplace_back(clampCoord(v[2 * i + 2]), clampCoord(v[2 * i + 3]));
        }
        if (s.points.size() + s.pointsF.size() + s.single.size() >= 2 * kSlice) {
            flush();
        }
    }
    flush();
}

// Crisp axis-aligned strokes at fractional DPR (M4 hidpi, D-209; 04 section 6.6 items 1 and 4, 06 section 17
// item 2). A line is logical width x DPR wide and a builder centres it where a 1 px line would sit (a pixel
// centre) or on a data position; at DPR 1.25 and 1.5 that gives 1.25, 1.5 or round(DPR) = 2 px strokes off
// the pixel grid, which the antialiased path smears over one pixel more than their width at partial coverage
// (blurred). As the backend already does for 1 px strokes (snapped to the pixel containing them) and for
// axis-aligned Quads and rectangles (whole pixels), a stroke of a list at a fractional DPR that is made only
// of horizontal and vertical segments and is wider than 1 px is drawn lround(width) pixels wide (at least 1)
// and centred so both its edges lie on pixel boundaries: across a segment a centre c becomes P + W / 2 with
// P = floor(c + (1 - W) / 2), the W pixels nearest to [c - W / 2, c + W / 2); along it a free end goes to the
// nearest pixel edge (crispEnd; a polyline's vertices are snapped both ways first, so its corners stay
// joined, and are mitred on the QPainter path). A Lines command is judged on all its segments; a stroke with
// a slanted segment (a trace, a trend line, a curved track) is drawn as listed, antialiased. The DrawList is
// unchanged (its JSON goldens stay what the builders emit), and so is every list at DPR 1 or 2, whose pixel
// goldens are authoritative (D-025): there the builders' 2 px lines on pixel centres (DPR 2 grid, cursor,
// ticks) stay as the goldens show them (docs/milestones/M4.md, hidpi open issues).
float crispCentre(float c, int widthPx) {
    const double first = std::floor(static_cast<double>(c) + 0.5 * static_cast<double>(1 - widthPx));
    return static_cast<float>(first + 0.5 * static_cast<double>(widthPx));
}

// A stroke's free end along its segment on the pixel edge the span rasteriser ends it at (the pixels whose
// centres lie before the end: ceil(e - 1/2)), so the QPainter path covers the same whole pixels.
float crispEnd(float e) {
    return static_cast<float>(std::ceil(static_cast<double>(e) - 0.5));
}

// The list and command to draw for `cmd`: `cmd` itself, or a snapped copy over this thread's scratch list.
struct StrokeToDraw {
    const plot::DrawList* list;
    plot::DrawCmd cmd;
    bool snapped = false;
};

bool fractionalDpr(const plot::DrawList& list) {
    const double dpr = static_cast<double>(list.dpr);
    return dpr > 0.0 && std::abs(dpr - std::round(dpr)) > 1e-6;
}

StrokeToDraw crispStroke(const plot::DrawList& list, const plot::DrawCmd& cmd) {
    // kMaxCrispWidthPx keeps lround in range (no builder draws a stroke anywhere near it).
    constexpr float kMaxCrispWidthPx = 1024.0F;
    if (!fractionalDpr(list) || !(cmd.widthPx > 1.0F) || !(cmd.widthPx <= kMaxCrispWidthPx) ||
        cmd.vertexCount == 0) {
        return {&list, cmd, false};
    }
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    const std::size_t n = cmd.vertexCount;
    const bool lines = cmd.kind == plot::CmdKind::Lines;
    const std::size_t step = lines ? 2 : 1;
    bool segment = false; // a dot (only zero-length segments) keeps its round shape
    for (std::size_t i = 0; i + 1 < n; i += step) {
        if (!finiteVertex(v, i) || !finiteVertex(v, i + 1)) {
            continue; // a gap: not a segment
        }
        const bool sameX = v[2 * i] == v[2 * i + 2];
        const bool sameY = v[2 * i + 1] == v[2 * i + 3];
        if (!sameX && !sameY) {
            return {&list, cmd, false}; // slanted
        }
        segment = segment || !(sameX && sameY);
    }
    if (!segment) {
        return {&list, cmd, false};
    }
    const int width = static_cast<int>(std::max(1L, std::lround(static_cast<double>(cmd.widthPx))));
    thread_local plot::DrawList snapped;
    snapped.verts.assign(v, v + 2 * n);
    float* out = snapped.verts.data();
    if (lines) {
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            if (!finiteVertex(v, i) || !finiteVertex(v, i + 1)) {
                continue;
            }
            const bool horizontal = v[2 * i + 1] == v[2 * i + 3];
            const bool vertical = v[2 * i] == v[2 * i + 2];
            for (const std::size_t k : {i, i + 1}) {
                // Across: the crisp centre; along: the end on a pixel edge (a flat end covers whole pixels).
                out[2 * k] = horizontal && !vertical ? crispEnd(v[2 * k]) : crispCentre(v[2 * k], width);
                out[2 * k + 1] =
                    vertical && !horizontal ? crispEnd(v[2 * k + 1]) : crispCentre(v[2 * k + 1], width);
            }
        }
    } else {
        // Every vertex both ways (the corners stay joined), then each run's free ends along their segment.
        for (std::size_t i = 0; i < n; ++i) {
            if (finiteVertex(v, i)) {
                out[2 * i] = crispCentre(v[2 * i], width);
                out[2 * i + 1] = crispCentre(v[2 * i + 1], width);
            }
        }
        const auto freeEnd = [&](std::size_t k, std::size_t towards) {
            if (v[2 * k + 1] == v[2 * towards + 1]) { // horizontal
                out[2 * k] = crispEnd(v[2 * k]);
            } else if (v[2 * k] == v[2 * towards]) { // vertical
                out[2 * k + 1] = crispEnd(v[2 * k + 1]);
            }
        };
        const auto differs = [&](std::size_t a, std::size_t b) {
            return v[2 * a] != v[2 * b] || v[2 * a + 1] != v[2 * b + 1];
        };
        std::size_t i = 0;
        while (i < n) {
            while (i < n && !finiteVertex(v, i)) {
                ++i;
            }
            std::size_t j = i;
            while (j < n && finiteVertex(v, j)) {
                ++j;
            }
            std::size_t first = i + 1;
            while (first < j && !differs(first, i)) {
                ++first;
            }
            if (first < j) {
                freeEnd(i, first);
                std::size_t last = j - 2;
                while (last > i && !differs(last, j - 1)) {
                    --last;
                }
                freeEnd(j - 1, last);
            }
            i = j;
        }
    }
    snapped.dpr = list.dpr;
    plot::DrawCmd c = cmd;
    c.vertexOffset = 0;
    c.widthPx = static_cast<float>(width);
    return {&snapped, c, true};
}

void drawPoints(QPainter& p, const plot::DrawList& list, const plot::DrawCmd& cmd) {
    const bool aliased = cmd.pointSizePx <= 1.0F;
    p.setRenderHint(QPainter::Antialiasing, !aliased);
    QPen pen(toQColor(cmd.colour));
    pen.setWidthF(aliased ? 1.0 : static_cast<qreal>(cmd.pointSizePx));
    pen.setCapStyle(aliased ? Qt::SquareCap : Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    Scratch& s = scratch();
    s.points.clear();
    s.pointsF.clear();
    const auto flush = [&] {
        if (aliased && !s.points.empty()) {
            p.drawPoints(s.points.data(), static_cast<int>(s.points.size()));
        } else if (!aliased && !s.pointsF.empty()) {
            p.drawPoints(s.pointsF.data(), static_cast<int>(s.pointsF.size()));
        }
        s.points.clear();
        s.pointsF.clear();
    };
    for (std::size_t i = 0; i < cmd.vertexCount; ++i) {
        if (!finiteVertex(v, i)) {
            continue;
        }
        if (aliased) {
            s.points.emplace_back(snapToPixel(v[2 * i]), snapToPixel(v[2 * i + 1]));
        } else {
            s.pointsF.emplace_back(clampCoord(v[2 * i]), clampCoord(v[2 * i + 1]));
        }
        if (s.points.size() + s.pointsF.size() >= kSlice) {
            flush();
        }
    }
    flush();
}

void drawQuads(QPainter& p, const plot::DrawList& list, const plot::DrawCmd& cmd) {
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    const QColor colour = toQColor(cmd.colour);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    for (std::size_t q = 0; q + 3 < cmd.vertexCount; q += 4) {
        bool finite = true;
        bool axisAligned = true;
        for (std::size_t k = 0; k < 4; ++k) {
            finite = finite && finiteVertex(v, q + k);
            const std::size_t a = q + k;
            const std::size_t b = q + (k + 1) % 4;
            if (v[2 * a] != v[2 * b] && v[2 * a + 1] != v[2 * b + 1]) {
                axisAligned = false;
            }
        }
        if (!finite) {
            continue;
        }
        if (axisAligned) {
            float x0 = v[2 * q];
            float x1 = x0;
            float y0 = v[2 * q + 1];
            float y1 = y0;
            for (std::size_t k = 1; k < 4; ++k) {
                x0 = std::min(x0, v[2 * (q + k)]);
                x1 = std::max(x1, v[2 * (q + k)]);
                y0 = std::min(y0, v[2 * (q + k) + 1]);
                y1 = std::max(y1, v[2 * (q + k) + 1]);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
            const QRect r = snapRect(plot::RectPx{x0, y0, x1 - x0, y1 - y0});
            if (!r.isEmpty()) {
                p.fillRect(r, colour);
            }
        } else {
            p.setRenderHint(QPainter::Antialiasing, true);
            const QPointF poly[4] = {{clampCoord(v[2 * q]), clampCoord(v[2 * q + 1])},
                                     {clampCoord(v[2 * q + 2]), clampCoord(v[2 * q + 3])},
                                     {clampCoord(v[2 * q + 4]), clampCoord(v[2 * q + 5])},
                                     {clampCoord(v[2 * q + 6]), clampCoord(v[2 * q + 7])}};
            p.drawPolygon(poly, 4);
        }
    }
}

std::string staticTextKey(const SchemeFonts& fonts, const plot::TextRun& run, qreal dpr,
                          const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    std::string key;
    key.reserve(16 + static_cast<std::size_t>(utf8.size()));
    const std::uint64_t id = fonts.id();
    const auto dprMilli = static_cast<std::uint32_t>(std::lround(dpr * 1000.0));
    key.append(reinterpret_cast<const char*>(&id), sizeof id);
    key.push_back(static_cast<char>(run.style));
    key.push_back(static_cast<char>(run.quarterTurns == 1 ? 1 : 0));
    const float scale = run.scale;
    key.append(reinterpret_cast<const char*>(&scale), sizeof scale);
    key.append(reinterpret_cast<const char*>(&dprMilli), sizeof dprMilli);
    key.append(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    return key;
}

// Text that Qt draws as outlines must not depend on the text drawn before it (04 section 6.7 item 1). Qt's
// raster engine fills a glyph whose device size exceeds its glyph-cache limit (pixel size^2 x |det| of the
// device transform > QT_MAX_CACHED_GLYPH_SIZE^2, 64 px by default; QPaintEngineEx::shouldDrawCachedGlyphs)
// with the outline from QFontEngine::addGlyphsToPath: the numeric gauge's dynamic font (TextRun::scale) at
// DPR 2. FreeType's engine loads those outlines under whatever transform the face holds, and the face (one
// per font file and thread, shared by every size) holds the subpixel offset of the glyph that thread last
// rasterised (QFontEngineFT::loadGlyph: FT_Set_Transform(face, matrix, offset)); with subpixel glyph
// positioning (slight or no hinting: fontconfig's hintslight default on Linux) the run then lands up to 3/4
// font unit off, so one gauge rendered twice in one process differed in 1,213 pixels (tst_golden_plots,
// ubuntu). Before such a run each face it is shaped with is reset: a new engine of the face
// (QRawFont::setPixelSize clones it; QFontEngineFT::init sets the transform without an offset) rasterises one
// glyph at subpixel 0 (loadGlyph sets the transform with a zero offset). Other font engines keep no such
// state; the reset then costs a clone and one small glyph, for large runs only.
constexpr qreal kOutlineResetPx = 8.0; // the clone's size: one small glyph to rasterise

qreal cachedGlyphLimitPx() {
    static const qreal limit = [] {
        const int env = qEnvironmentVariableIntValue("QT_MAX_CACHED_GLYPH_SIZE"); // as Qt reads it
        return static_cast<qreal>(env > 0 ? env : 64);
    }();
    return limit;
}

// Whether Qt may draw the painter's font as outlines under the painter's transform: its device size against
// three quarters of the limit (Qt resolves the pixel size with its own rounding).
bool mayDrawAsOutlines(const QPainter& p) {
    const QFont& f = p.font();
    const qreal dpi = p.device() != nullptr ? static_cast<qreal>(p.device()->logicalDpiY()) : 96.0;
    const qreal px = f.pixelSize() > 0 ? static_cast<qreal>(f.pixelSize()) : f.pointSizeF() * dpi / 72.0;
    const qreal limit = 0.75 * cachedGlyphLimitPx();
    return px * px * std::abs(p.deviceTransform().determinant()) > limit * limit;
}

void resetOutlineState(const QFont& font, const QString& text) {
    QTextLayout layout(text, font);
    layout.beginLayout();
    layout.createLine(); // unbounded: the whole text on one line
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    for (const QGlyphRun& run : runs) {
        QRawFont face = run.rawFont();
        if (!face.isValid()) {
            continue;
        }
        face.setPixelSize(qFuzzyCompare(face.pixelSize(), kOutlineResetPx) ? 2.0 * kOutlineResetPx
                                                                           : kOutlineResetPx);
        const QList<quint32> glyphs = run.glyphIndexes();
        static_cast<void>(
            face.alphaMapForGlyph(glyphs.isEmpty() ? 0U : glyphs.front(), QRawFont::PixelAntialiasing));
    }
}

void drawText(QPainter& p, const plot::DrawList& list, const plot::DrawCmd& cmd, const SchemeFonts& fonts,
              qreal dpr, const QTransform& base, const QTransform& device) {
    if (cmd.textIndex >= list.texts.size()) {
        return;
    }
    const plot::TextRun& run = list.texts[cmd.textIndex];
    const detail::FontEntry& font = detail::runFontEntry(fonts, run);
    const auto tp = detail::placeText(list, run, font, dpr);
    if (!tp || tp->text.isEmpty()) {
        return;
    }
    p.setWorldTransform(tp->frame() * base);
    if (project::alpha(run.background) != 0) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(tp->box, toQColor(run.background));
    }
    p.setFont(font.font);
    p.setPen(toQColor(run.colour));
    // A named key: the reference is into this thread's cache, but GCC 13's -Wdangling-reference cannot tell
    // that from a call whose argument is a temporary.
    const std::string key = staticTextKey(fonts, run, dpr, tp->text);
    bool newText = false;
    const QStaticText& st = detail::threadStaticText(key, tp->text, &newText);
    // One subpixel bin count for every engine this text is drawn with, before Qt computes one from this
    // thread's first glyph-cache fill at whatever DPR that had (glyph_bins.hpp; on by default on macOS only,
    // D-220).
    detail::pinGlyphSubpixelBins(p.font(), tp->text, newText);
    if (mayDrawAsOutlines(p)) {
        resetOutlineState(font.font, tp->text); // nothing rasterises a glyph between this and the outlines
    }
    p.drawStaticText(tp->box.topLeft(), st);
    p.setWorldTransform(device);
}

// The Static-layer fast path (span_raster.hpp): solid strokes of width <= SpanRaster::kMaxWidthPx straight
// into the layer's pixels. Only for a layer painted from its origin at the list's DPR.
struct FastTarget {
    uchar* bits = nullptr;
    qsizetype bytesPerLine = 0;
    QRect bounds; // the layer, device px
    QRect clip;   // bounds intersected with the current Scissor
};

bool fastEligible(const FastTarget* fast, const plot::DrawCmd& cmd) {
    return fast != nullptr && cmd.dash == plot::DashId::Solid &&
           cmd.widthPx <= detail::SpanRaster::kMaxWidthPx;
}

// Vertical strips of a parallel Static rasterisation (RasterBackend::rasteriseStatic): the columns each
// command reaches, measured once, so a strip skips the commands outside its columns and walks only the part
// of an x-monotonic trace polyline that reaches it.
struct StripCommand {
    float x0 = 0.0F;        // device px, conservative (stroke width, antialiasing spill)
    float x1 = -1.0F;       // x1 < x0: paints nothing
    bool every = false;     // Clear, Scissor (clip state) and whole-layer commands: every strip
    bool monotonic = false; // a polyline of finite vertices whose x never decreases
};

struct StripFilter {
    int x0 = 0; // the strip's columns [x0, x1) of rows [0, height), device px of the whole layer
    int x1 = 0;
    int height = 0;
    const std::vector<StripCommand>* commands = nullptr;
    QRect columns() const { return QRect(x0, 0, x1 - x0, height); }
    bool skips(std::size_t index) const {
        const StripCommand& c = (*commands)[index];
        return !c.every && (c.x1 < c.x0 || c.x1 < static_cast<float>(x0) || c.x0 >= static_cast<float>(x1));
    }
};

void fastPolyline(const FastTarget& fast, const plot::DrawList& list, const plot::DrawCmd& cmd,
                  const StripFilter* strip = nullptr, std::size_t index = 0) {
    if (fast.clip.isEmpty()) {
        return;
    }
    detail::SpanRaster raster(fast.bits, fast.bytesPerLine, fast.clip, cmd.colour);
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    std::size_t n = cmd.vertexCount;
    if (strip != nullptr && (*strip->commands)[index].monotonic && project::alpha(cmd.colour) == 0xFF) {
        // Opaque and x-monotonic: only the vertices from the last one left of the strip (with the stroke's
        // reach) to the first one right of it; every segment touching the strip is drawn, so its pixels are
        // those of the whole polyline.
        const float reach = std::max(cmd.widthPx, 1.0F) + 2.0F;
        const float left = static_cast<float>(strip->x0) - reach;
        const float right = static_cast<float>(strip->x1) + reach;
        std::size_t lo = 0;
        std::size_t hi = n;
        while (lo < hi) { // first vertex with x >= left
            const std::size_t mid = lo + (hi - lo) / 2;
            if (v[2 * mid] < left) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        std::size_t end = lo;
        hi = n;
        while (end < hi) { // first vertex with x > right
            const std::size_t mid = end + (hi - end) / 2;
            if (v[2 * mid] <= right) {
                end = mid + 1;
            } else {
                hi = mid;
            }
        }
        const std::size_t first = lo > 0 ? lo - 1 : 0;
        const std::size_t last = std::min(n, end + 1);
        if (last <= first + 1 && !(last == first + 1 && n == 1)) {
            return;
        }
        v += 2 * first;
        n = last - first;
    }
    std::size_t i = 0;
    while (i < n) {
        while (i < n && !finiteVertex(v, i)) {
            ++i;
        }
        std::size_t j = i;
        while (j < n && finiteVertex(v, j)) {
            ++j;
        }
        if (j > i) {
            if (cmd.widthPx <= 1.0F) {
                raster.aliasedPolyline(v + 2 * i, j - i);
            } else {
                raster.thickPolyline(v + 2 * i, j - i, cmd.widthPx);
            }
        }
        i = j;
    }
}

void fastLines(const FastTarget& fast, const plot::DrawList& list, const plot::DrawCmd& cmd) {
    if (fast.clip.isEmpty()) {
        return;
    }
    detail::SpanRaster raster(fast.bits, fast.bytesPerLine, fast.clip, cmd.colour);
    const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
    for (std::size_t i = 0; i + 1 < cmd.vertexCount; i += 2) {
        if (!finiteVertex(v, i) || !finiteVertex(v, i + 1)) {
            continue;
        }
        if (cmd.widthPx <= 1.0F) {
            raster.aliasedSegment(v[2 * i], v[2 * i + 1], v[2 * i + 2], v[2 * i + 3]);
        } else {
            raster.thickSegment(v[2 * i], v[2 * i + 1], v[2 * i + 2], v[2 * i + 3], cmd.widthPx);
        }
    }
}

void paintCommands(QPainter& painter, const plot::DrawList& list, const SchemeFonts& fonts, FastTarget* fast,
                   const StripFilter* strip = nullptr) {
    if (list.cmds.empty() || !painter.isActive()) {
        return;
    }
    const qreal dpr = list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : 1.0;
    painter.save();
    const QTransform base = painter.worldTransform();
    const QTransform device = QTransform::fromScale(1.0 / dpr, 1.0 / dpr) * base;
    const bool baseClipping = painter.hasClipping();
    const QRegion baseClip = baseClipping ? painter.clipRegion() : QRegion();
    if (fast != nullptr) {
        fast->clip = fast->bounds;
    }
    painter.setWorldTransform(device);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (strip != nullptr) {
        // A strip paints in the whole layer's coordinates, exactly those of one pass, clipped to its columns
        // (device px under `device`). Never a translated view of the columns: QPainter::drawStaticText
        // converts the run's device position to 26.6 fixed point by truncation toward zero, so a run that
        // starts left of a translated origin lands 1/64 px further right, and with subpixel glyph positioning
        // (FreeType with slight or no hinting, as fontconfig sets on Linux, in quarter pixels; CoreText) can
        // take another glyph image than in one pass.
        painter.setClipRect(strip->columns(), Qt::ReplaceClip);
    }

    for (std::size_t index = 0; index < list.cmds.size(); ++index) {
        const plot::DrawCmd& cmd = list.cmds[index];
        if (strip != nullptr && strip->skips(index)) {
            continue;
        }
        switch (cmd.kind) {
        case plot::CmdKind::Clear: {
            const QRect layer = list.widthPx > 0 && list.heightPx > 0
                                    ? QRect(0, 0, list.widthPx, list.heightPx)
                                    : QRect(0, 0, 1 << 20, 1 << 20);
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(layer, toQColor(cmd.colour));
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            break;
        }
        case plot::CmdKind::Scissor:
            if (strip != nullptr) {
                const QRect columns = strip->columns();
                painter.setClipRect(cmd.rect.w > 0.0F ? columns.intersected(snapRect(cmd.rect)) : columns,
                                    Qt::ReplaceClip);
            } else {
                painter.setWorldTransform(base);
                if (baseClipping) {
                    painter.setClipRegion(baseClip);
                } else {
                    painter.setClipping(false);
                }
                painter.setWorldTransform(device);
                if (cmd.rect.w > 0.0F) {
                    painter.setClipRect(snapRect(cmd.rect),
                                        baseClipping ? Qt::IntersectClip : Qt::ReplaceClip);
                }
            }
            if (fast != nullptr) {
                fast->clip = cmd.rect.w > 0.0F ? fast->bounds.intersected(snapRect(cmd.rect)) : fast->bounds;
            }
            break;
        case plot::CmdKind::Polyline:
            if (vertexRangeOk(list, cmd)) {
                const StrokeToDraw s = crispStroke(list, cmd);
                if (fastEligible(fast, s.cmd)) {
                    fastPolyline(*fast, *s.list, s.cmd, strip, index);
                } else {
                    drawPolyline(painter, *s.list, s.cmd, s.snapped);
                }
            }
            break;
        case plot::CmdKind::Lines:
            if (vertexRangeOk(list, cmd)) {
                const StrokeToDraw s = crispStroke(list, cmd);
                if (fastEligible(fast, s.cmd)) {
                    fastLines(*fast, *s.list, s.cmd);
                } else {
                    drawLines(painter, *s.list, s.cmd, s.snapped);
                }
            }
            break;
        case plot::CmdKind::Points:
            if (vertexRangeOk(list, cmd)) {
                drawPoints(painter, list, cmd);
            }
            break;
        case plot::CmdKind::Quads:
            if (vertexRangeOk(list, cmd)) {
                drawQuads(painter, list, cmd);
            }
            break;
        case plot::CmdKind::FilledRect: {
            const QRect r = snapRect(cmd.rect);
            if (!r.isEmpty()) {
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.fillRect(r, toQColor(cmd.colour));
            }
            break;
        }
        case plot::CmdKind::TextRun:
            drawText(painter, list, cmd, fonts, dpr, base, device);
            break;
        }
    }
    painter.restore();
}

} // namespace

namespace detail {

namespace {

// The columns commands [begin, end) of a Static list reach (commandBounds; a polyline's own scan also tells
// whether it is x-monotonic), into out[begin, end).
void measureStripCommands(const plot::DrawList& list, const SchemeFonts& fonts, std::size_t begin,
                          std::size_t end, std::vector<StripCommand>& out) {
    for (std::size_t i = begin; i < end; ++i) {
        const plot::DrawCmd& cmd = list.cmds[i];
        StripCommand& c = out[i];
        if (cmd.kind == plot::CmdKind::Scissor) {
            c.every = true;
            continue;
        }
        if (cmd.kind == plot::CmdKind::Polyline) {
            if (!vertexRangeOk(list, cmd)) {
                continue;
            }
            const float* v = list.verts.data() + static_cast<std::size_t>(cmd.vertexOffset) * 2;
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            bool monotonic = true;
            float previous = std::numeric_limits<float>::lowest();
            for (std::uint32_t k = 0; k < cmd.vertexCount; ++k) {
                const float x = v[2 * k];
                if (!std::isfinite(x) || !std::isfinite(v[2 * k + 1])) {
                    monotonic = false;
                    continue;
                }
                monotonic = monotonic && x >= previous;
                previous = x;
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
            if (lo <= hi) {
                const float reach = std::max(cmd.widthPx, 1.0F) + 2.0F;
                c.x0 = lo - reach;
                c.x1 = hi + reach + 1.0F;
                c.monotonic = monotonic;
            }
            continue;
        }
        const detail::CommandBounds b = detail::commandBounds(list, cmd, fonts);
        if (b.wholeLayer) {
            c.every = true;
        } else if (!b.rect.isEmpty()) {
            // A text run's box is its advance by ascent + descent, but glyph ink may reach past it (negative
            // side bearings, accents, an unhinted glyph's antialiased edge at a subpixel offset): a run is
            // also painted, clipped, by the strips within one line height of its box.
            qreal spill = 0.0;
            if (cmd.kind == plot::CmdKind::TextRun) {
                const plot::TextRun& run = list.texts[cmd.textIndex]; // in range: the bounds are not empty
                const qreal dpr = list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : 1.0;
                spill = std::ceil(runFontEntry(fonts, run).metrics.height() * dpr);
            }
            c.x0 = static_cast<float>(std::floor(b.rect.left() - spill));
            c.x1 = static_cast<float>(std::ceil(b.rect.right() + spill));
        }
    }
}

} // namespace

// Columns [x0, x1) of a Static layer of `width` x `height` device px whose pixels start at `bits`: a QImage
// over the whole layer (no copy; the painter's coordinates are those of one pass) clipped to the columns, and
// the direct rasteriser clipped to them; the commands outside the columns are skipped. Strips running at once
// share the pixels but each reads and writes only its own columns.
void paintStaticStrip(uchar* bits, qsizetype bytesPerLine, int width, int height, int x0, int x1, qreal dpr,
                      const plot::DrawList& list, const SchemeFonts& fonts,
                      const std::vector<StripCommand>& commands) {
    QImage view(bits, width, height, bytesPerLine, QImage::Format_ARGB32_Premultiplied);
    view.setDevicePixelRatio(dpr);
    FastTarget fast;
    fast.bits = bits;
    fast.bytesPerLine = bytesPerLine;
    fast.bounds = QRect(x0, 0, x1 - x0, height);
    const StripFilter filter{x0, x1, height, &commands};
    QPainter painter(&view);
    paintCommands(painter, list, fonts, &fast, &filter);
}

namespace {
std::mutex& spareMutex() {
    static std::mutex m;
    return m;
}
std::vector<QImage>& spareBuffers() {
    static std::vector<QImage> v;
    return v;
}
} // namespace

void recycleStaticBuffer(QImage image) {
    if (image.isNull() || image.format() != QImage::Format_ARGB32_Premultiplied || !image.isDetached()) {
        return;
    }
    const std::scoped_lock lock(spareMutex());
    std::vector<QImage>& spare = spareBuffers();
    spare.insert(spare.begin(), std::move(image)); // most recent first
    if (spare.size() > kSpareStaticBuffers) {
        spare.pop_back();
    }
}

QImage takeStaticBuffer(QSize size) {
    const std::scoped_lock lock(spareMutex());
    std::vector<QImage>& spare = spareBuffers();
    const auto it =
        std::find_if(spare.begin(), spare.end(), [&](const QImage& i) { return i.size() == size; });
    if (it == spare.end()) {
        return {};
    }
    QImage out = std::move(*it);
    spare.erase(it);
    return out;
}

void paintStaticList(QImage& image, const plot::DrawList& list, const SchemeFonts& fonts) {
    if (image.isNull()) {
        return;
    }
    const qreal dpr = list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : 1.0;
    const bool direct = image.format() == QImage::Format_ARGB32_Premultiplied &&
                        std::abs(image.devicePixelRatio() - dpr) < 1e-9;
    FastTarget fast;
    if (direct) {
        fast.bits = image.bits(); // detaches now, so QPainter::begin below paints this very buffer
        fast.bytesPerLine = image.bytesPerLine();
        fast.bounds = image.rect();
    }
    QPainter painter(&image);
    paintCommands(painter, list, fonts, direct ? &fast : nullptr);
}

} // namespace detail

QImage RasterBackend::rasteriseStatic(const plot::DrawList& list, const SchemeFonts& fonts, plot::Rgba clear,
                                      TaskPool* pool) {
    if (list.widthPx <= 0 || list.heightPx <= 0) {
        return {};
    }
    const qreal dpr = list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : 1.0;
    QImage image = detail::takeStaticBuffer(QSize(list.widthPx, list.heightPx));
    if (image.isNull()) {
        image = QImage(list.widthPx, list.heightPx, QImage::Format_ARGB32_Premultiplied);
    }
    if (image.isNull()) {
        return {};
    }
    image.setDevicePixelRatio(dpr);
    const bool clearsFirst = !list.cmds.empty() && list.cmds.front().kind == plot::CmdKind::Clear;
    uchar* bits = image.bits();
    const qsizetype bpl = image.bytesPerLine();
    const int width = list.widthPx;
    const int height = list.heightPx;
    const double unitD = std::round(dpr);
    const bool wholeDpr = std::abs(dpr - unitD) < 1e-9 && unitD >= 1.0;
    if (pool == nullptr) {
        pool = TaskPool::currentWorkerPool();
    }
    int strips = 1;
    if (pool != nullptr && wholeDpr && !ThreadRole::isNoBlock()) {
        // Twice the workers, so the text-heavy key column's strip does not hold the others up.
        const auto workers = static_cast<int>(pool->stats().workers);
        strips = std::clamp(std::min(2 * workers, width / kMinStripColumns), 1, kMaxStaticStrips);
    }
    const QColor fill = detail::toQColor(clear);
    if (strips == 1) {
        if (!clearsFirst) {
            image.fill(fill);
        }
        detail::paintStaticList(image, list, fonts);
        return image;
    }
    // Strip edges on 64-byte cache lines (16 device px per logical pixel unit, so also on whole logical
    // pixels); every strip paints in the layer's own coordinates (paintStaticStrip), so the edges never move
    // what is drawn.
    const int align = 16 * static_cast<int>(unitD);
    const int columns = ((width + strips - 1) / strips + align - 1) / align * align;
    // The commands' columns, measured in parallel slices of about equal work (a text run's bounds need its
    // font metrics; a polyline is a scan of its vertices).
    std::vector<StripCommand> commands(list.cmds.size());
    {
        const auto cost = [](const plot::DrawCmd& c) -> std::size_t {
            return c.kind == plot::CmdKind::TextRun ? 2000U : 16U + c.vertexCount;
        };
        std::size_t total = 0;
        for (const plot::DrawCmd& c : list.cmds) {
            total += cost(c);
        }
        const std::size_t share = total / static_cast<std::size_t>(strips) + 1;
        std::vector<std::pair<std::size_t, std::size_t>> slices;
        std::size_t begin = 0;
        std::size_t acc = 0;
        for (std::size_t i = 0; i < list.cmds.size(); ++i) {
            acc += cost(list.cmds[i]);
            if (acc >= share) {
                slices.emplace_back(begin, i + 1);
                begin = i + 1;
                acc = 0;
            }
        }
        if (begin < list.cmds.size()) {
            slices.emplace_back(begin, list.cmds.size());
        }
        std::vector<Future<int>> measured;
        for (std::size_t k = 1; k < slices.size(); ++k) {
            measured.push_back(pool->submit(
                Priority::Visible, TaskKey{}, CancelToken{},
                [&list, &fonts, &commands, slice = slices[k]](const CancelToken&) {
                    detail::measureStripCommands(list, fonts, slice.first, slice.second, commands);
                    return 0;
                }));
        }
        if (!slices.empty()) {
            detail::measureStripCommands(list, fonts, slices.front().first, slices.front().second, commands);
        }
        for (Future<int>& f : measured) {
            static_cast<void>(std::move(f).take());
        }
    }
    const auto paintStrip = [&](int x0, int x1) {
        if (!clearsFirst) {
            QImage view(bits + static_cast<qsizetype>(x0) * 4, x1 - x0, height, bpl,
                        QImage::Format_ARGB32_Premultiplied);
            view.fill(fill);
        }
        detail::paintStaticStrip(bits, bpl, width, height, x0, x1, dpr, list, fonts, commands);
    };
    std::vector<Future<int>> parts;
    for (int x0 = columns; x0 < width; x0 += columns) {
        const int x1 = std::min(width, x0 + columns);
        parts.push_back(pool->submit(Priority::Visible, TaskKey{}, CancelToken{},
                                     [&paintStrip, x0, x1](const CancelToken&) {
                                         paintStrip(x0, x1);
                                         return 0;
                                     }));
    }
    paintStrip(0, std::min(width, columns));
    for (Future<int>& f : parts) {
        static_cast<void>(std::move(f).take()); // every strip is painted before the image leaves (Rule W)
    }
    return image;
}

RasterBackend::RasterBackend(std::shared_ptr<const SchemeFonts> fonts,
                             std::shared_ptr<const plot::ITextMetrics> metrics)
    : fonts_(std::move(fonts)), metrics_(std::move(metrics)) {}

RasterBackend::~RasterBackend() = default;

const SchemeFonts& RasterBackend::fonts() const {
    return fonts_ ? *fonts_ : detail::defaultSchemeFonts();
}

void RasterBackend::beginLayer(plot::LayerKind, int widthPx, int heightPx, float dpr, plot::Rgba clear) {
    if (current_ != 0) {
        release(plot::LayerHandle{current_});
    }
    const int w = std::max(widthPx, 0);
    const int h = std::max(heightPx, 0);
    QImage img;
    const auto it = std::find_if(free_.begin(), free_.end(),
                                 [&](const QImage& f) { return f.width() == w && f.height() == h; });
    if (it != free_.end()) {
        img = std::move(*it);
        free_.erase(it);
    } else if (w > 0 && h > 0) {
        img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    }
    if (!img.isNull()) {
        img.setDevicePixelRatio(dpr > 0.0F ? static_cast<qreal>(dpr) : 1.0);
        img.fill(toQColor(clear));
    }
    current_ = nextId_++;
    if (nextId_ == 0) {
        nextId_ = 1;
    }
    layers_[current_] = std::move(img);
}

void RasterBackend::draw(const plot::DrawList& list) {
    if (current_ == 0) {
        return;
    }
    detail::paintStaticList(layers_[current_], list, fonts());
}

plot::LayerHandle RasterBackend::endLayer() {
    const plot::LayerHandle h{current_};
    current_ = 0;
    return h;
}

void RasterBackend::compose(std::span<const plot::LayerHandle> inOrder, plot::LayerHandle target) {
    const auto t = layers_.find(target.id);
    if (t == layers_.end() || t->second.isNull()) {
        return;
    }
    QPainter painter(&t->second);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const plot::LayerHandle& h : inOrder) {
        if (h.id == target.id) {
            continue;
        }
        const auto it = layers_.find(h.id);
        if (it != layers_.end() && !it->second.isNull()) {
            painter.drawImage(QPointF(0.0, 0.0), it->second);
        }
    }
}

plot::Bitmap RasterBackend::readback(plot::LayerHandle layer) {
    plot::Bitmap b;
    const auto it = layers_.find(layer.id);
    if (it == layers_.end() || it->second.isNull()) {
        return b;
    }
    const QImage straight = it->second.convertToFormat(QImage::Format_RGBA8888);
    b.w = straight.width();
    b.h = straight.height();
    const auto rowBytes = static_cast<std::size_t>(b.w) * 4;
    b.rgba.resize(rowBytes * static_cast<std::size_t>(b.h));
    for (int y = 0; y < b.h; ++y) {
        std::memcpy(b.rgba.data() + rowBytes * static_cast<std::size_t>(y), straight.constScanLine(y),
                    rowBytes);
    }
    return b;
}

void RasterBackend::release(plot::LayerHandle layer) {
    const auto it = layers_.find(layer.id);
    if (it == layers_.end()) {
        return;
    }
    if (!it->second.isNull() && free_.size() < kMaxFreeLayers) {
        free_.push_back(std::move(it->second));
    }
    layers_.erase(it);
    if (layer.id == current_) {
        current_ = 0;
    }
}

QImage RasterBackend::image(plot::LayerHandle layer) const {
    const auto it = layers_.find(layer.id);
    return it == layers_.end() ? QImage() : it->second;
}

void RasterBackend::paintList(QPainter& painter, const plot::DrawList& list, const SchemeFonts& fonts) {
    paintCommands(painter, list, fonts, nullptr);
}

} // namespace esses::qt
