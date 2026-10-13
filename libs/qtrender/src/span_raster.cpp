// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "span_raster.hpp"

#include <QColor>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace esses::qt::detail {

namespace {

constexpr double kCoordLimit = 1.0e6;

// x * a / 255 per channel of a premultiplied ARGB32 value (the classic two-channels-at-a-time multiply).
inline quint32 byteMul(quint32 x, unsigned a) {
    quint32 t = (x & 0xff00ffU) * a;
    t = (t + ((t >> 8) & 0xff00ffU) + 0x800080U) >> 8;
    t &= 0xff00ffU;
    x = ((x >> 8) & 0xff00ffU) * a;
    x = (x + ((x >> 8) & 0xff00ffU) + 0x800080U);
    x &= 0xff00ff00U;
    return x | t;
}

inline int floorToInt(double v) {
    return static_cast<int>(std::floor(std::clamp(v, -kCoordLimit, kCoordLimit)));
}

// DrawList vertices are float device px; widening to double is exact.
inline int floorToInt(float v) {
    return floorToInt(static_cast<double>(v));
}

inline QPoint snap(const float* xy, std::size_t i) {
    return QPoint(floorToInt(xy[2 * i]), floorToInt(xy[2 * i + 1]));
}

} // namespace

SpanRaster::SpanRaster(uchar* bits, qsizetype bytesPerLine, QRect clip, project::Rgba colour)
    : bits_(bits), bpl_(bytesPerLine), clip_(clip),
      premul_(qPremultiply(qRgba(project::red(colour), project::green(colour), project::blue(colour),
                                 project::alpha(colour)))),
      opaque_(project::alpha(colour) == 0xFF) {}

void SpanRaster::blend(int x, int y, unsigned coverage) {
    auto* p = reinterpret_cast<quint32*>(bits_ + bpl_ * y) + x;
    if (coverage >= 255 && opaque_) {
        *p = premul_;
        return;
    }
    const quint32 s = coverage >= 255 ? premul_ : byteMul(premul_, coverage);
    *p = s + byteMul(*p, 255U - qAlpha(s));
}

void SpanRaster::vspan(int x, int y0, int y1) {
    if (x < clip_.left() || x > clip_.right()) {
        return;
    }
    if (y0 > y1) {
        std::swap(y0, y1);
    }
    y0 = std::max(y0, clip_.top());
    y1 = std::min(y1, clip_.bottom());
    if (y0 > y1) {
        return;
    }
    uchar* row = bits_ + bpl_ * y0 + static_cast<qsizetype>(x) * 4;
    if (opaque_) {
        for (int y = y0; y <= y1; ++y, row += bpl_) {
            *reinterpret_cast<quint32*>(row) = premul_;
        }
        return;
    }
    const unsigned inverse = 255U - qAlpha(premul_);
    for (int y = y0; y <= y1; ++y, row += bpl_) {
        auto* p = reinterpret_cast<quint32*>(row);
        *p = premul_ + byteMul(*p, inverse);
    }
}

// A vertical stroke: the same coverage pattern (columns xa..xb) on rows y0..y1, precomputed once.
void SpanRaster::verticalRun(double a, double b, int y0, int y1) {
    y0 = std::max(y0, clip_.top());
    y1 = std::min(y1, clip_.bottom());
    const int first = std::max(floorToInt(a), clip_.left());
    const int last = std::min(floorToInt(b), clip_.right());
    if (y0 > y1 || first > last || last - first >= 16) {
        for (int y = y0; y <= y1; ++y) {
            coverRow(y, a, b);
        }
        return;
    }
    quint32 src[16];
    unsigned inv[16];
    bool store[16];
    const int count = last - first + 1;
    for (int k = 0; k < count; ++k) {
        const int x = first + k;
        const double cov = std::min(b, x + 1.0) - std::max(a, static_cast<double>(x));
        const unsigned c = cov >= 1.0   ? 255U
                           : cov <= 0.0 ? 0U
                                        : static_cast<unsigned>(std::lround(cov * 255.0));
        src[k] = c >= 255U ? premul_ : byteMul(premul_, c);
        inv[k] = 255U - qAlpha(src[k]);
        store[k] = c >= 255U && opaque_;
    }
    for (int y = y0; y <= y1; ++y) {
        auto* p = reinterpret_cast<quint32*>(bits_ + bpl_ * y) + first;
        for (int k = 0; k < count; ++k) {
            p[k] = store[k] ? src[k] : src[k] + byteMul(p[k], inv[k]);
        }
    }
}

void SpanRaster::hspan(int y, int x0, int x1) {
    if (y < clip_.top() || y > clip_.bottom()) {
        return;
    }
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    x0 = std::max(x0, clip_.left());
    x1 = std::min(x1, clip_.right());
    if (x0 > x1) {
        return;
    }
    auto* p = reinterpret_cast<quint32*>(bits_ + bpl_ * y);
    if (opaque_) {
        std::fill(p + x0, p + x1 + 1, premul_);
        return;
    }
    for (int x = x0; x <= x1; ++x) {
        blend(x, y, 255);
    }
}

void SpanRaster::line(QPoint a, QPoint b, bool includeStart, bool includeEnd) {
    if (a == b) {
        if ((includeStart || includeEnd) && clip_.contains(a)) {
            blend(a.x(), a.y(), 255);
        }
        return;
    }
    if (std::max(a.x(), b.x()) < clip_.left() || std::min(a.x(), b.x()) > clip_.right() ||
        std::max(a.y(), b.y()) < clip_.top() || std::min(a.y(), b.y()) > clip_.bottom()) {
        return;
    }
    if (a.x() == b.x()) {
        const int dir = b.y() > a.y() ? 1 : -1;
        const int from = includeStart ? a.y() : a.y() + dir;
        const int to = includeEnd ? b.y() : b.y() - dir;
        if ((to - from) * dir >= 0) {
            vspan(a.x(), from, to);
        }
        return;
    }
    if (a.y() == b.y()) {
        const int dir = b.x() > a.x() ? 1 : -1;
        const int from = includeStart ? a.x() : a.x() + dir;
        const int to = includeEnd ? b.x() : b.x() - dir;
        if ((to - from) * dir >= 0) {
            hspan(a.y(), from, to);
        }
        return;
    }
    // Bresenham (all octants). Off-layer stretches are stepped through; vertices are clamped to +-1e6 and
    // builders clamp to the panel, so the loop stays bounded.
    const int dx = std::abs(b.x() - a.x());
    const int sx = a.x() < b.x() ? 1 : -1;
    const int dy = -std::abs(b.y() - a.y());
    const int sy = a.y() < b.y() ? 1 : -1;
    int err = dx + dy;
    int x = a.x();
    int y = a.y();
    bool atStart = true;
    for (;;) {
        const bool atEnd = x == b.x() && y == b.y();
        const bool draw = atEnd ? includeEnd : (!atStart || includeStart);
        if (draw && clip_.contains(x, y)) {
            blend(x, y, 255);
        }
        if (atEnd) {
            break;
        }
        atStart = false;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void SpanRaster::aliasedPolyline(const float* xy, std::size_t n) {
    if (n == 0) {
        return;
    }
    QPoint prev = snap(xy, 0);
    bool pending = false; // a vertical run in one pixel column, drawn as one span
    int px = 0;
    int plo = 0;
    int phi = 0;
    bool lastInPending = false;
    bool startDrawn = false; // the next segment's start pixel belongs to the span just drawn
    for (std::size_t i = 1; i < n; ++i) {
        const QPoint cur = snap(xy, i);
        if (cur == prev) {
            continue;
        }
        if (cur.x() == prev.x()) {
            if (pending && px == cur.x()) {
                plo = std::min({plo, prev.y(), cur.y()});
                phi = std::max({phi, prev.y(), cur.y()});
            } else {
                if (pending) {
                    vspan(px, plo, phi);
                }
                pending = true;
                px = cur.x();
                plo = std::min(prev.y(), cur.y());
                phi = std::max(prev.y(), cur.y());
            }
            lastInPending = true;
        } else {
            if (pending) {
                vspan(px, plo, phi);
                pending = false;
                startDrawn = true;
            }
            line(prev, cur, !startDrawn, false);
            startDrawn = false;
            lastInPending = false;
        }
        prev = cur;
    }
    if (pending) {
        vspan(px, plo, phi);
    }
    if (!lastInPending && clip_.contains(prev)) {
        blend(prev.x(), prev.y(), 255); // the polyline's last pixel (or its only one)
    }
}

void SpanRaster::aliasedSegment(float x0, float y0, float x1, float y1) {
    line(QPoint(floorToInt(x0), floorToInt(y0)), QPoint(floorToInt(x1), floorToInt(y1)), true, true);
}

void SpanRaster::coverRow(int y, double a, double b) {
    if (y < clip_.top() || y > clip_.bottom() || !(b > a)) {
        return;
    }
    const int first = std::max(floorToInt(a), clip_.left());
    const int last = std::min(floorToInt(b), clip_.right());
    for (int x = first; x <= last; ++x) {
        const double cov = std::min(b, x + 1.0) - std::max(a, static_cast<double>(x));
        if (cov > 0.0) {
            blend(x, y, cov >= 1.0 ? 255U : static_cast<unsigned>(std::lround(cov * 255.0)));
        }
    }
}

void SpanRaster::coverColumn(int x, double a, double b) {
    if (x < clip_.left() || x > clip_.right() || !(b > a)) {
        return;
    }
    const int first = std::max(floorToInt(a), clip_.top());
    const int last = std::min(floorToInt(b), clip_.bottom());
    for (int y = first; y <= last; ++y) {
        const double cov = std::min(b, y + 1.0) - std::max(a, static_cast<double>(y));
        if (cov > 0.0) {
            blend(x, y, cov >= 1.0 ? 255U : static_cast<unsigned>(std::lround(cov * 255.0)));
        }
    }
}

// Pixel rows (steep) or columns (shallow) whose centre c has t = (c - start) / delta in [0, 1).
void SpanRaster::thick(double x0, double y0, double x1, double y1, double widthPx) {
    x0 = std::clamp(x0, -kCoordLimit, kCoordLimit);
    y0 = std::clamp(y0, -kCoordLimit, kCoordLimit);
    x1 = std::clamp(x1, -kCoordLimit, kCoordLimit);
    y1 = std::clamp(y1, -kCoordLimit, kCoordLimit);
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        return;
    }
    const double h = widthPx / 2.0;
    if (std::abs(dy) >= std::abs(dx)) {
        int first = 0;
        int last = 0;
        if (dy > 0.0) {
            first = static_cast<int>(std::ceil(y0 - 0.5));
            last = static_cast<int>(std::ceil(y1 - 0.5)) - 1;
        } else {
            first = static_cast<int>(std::floor(y1 - 0.5)) + 1;
            last = static_cast<int>(std::floor(y0 - 0.5));
        }
        const double he = h * len / std::abs(dy);
        if (dx == 0.0) {
            verticalRun(x0 - he, x0 + he, first, last);
            return;
        }
        first = std::max(first, clip_.top());
        last = std::min(last, clip_.bottom());
        const double slope = dx / dy;
        for (int r = first; r <= last; ++r) {
            const double xc = x0 + (r + 0.5 - y0) * slope;
            coverRow(r, xc - he, xc + he);
        }
    } else {
        int first = 0;
        int last = 0;
        if (dx > 0.0) {
            first = static_cast<int>(std::ceil(x0 - 0.5));
            last = static_cast<int>(std::ceil(x1 - 0.5)) - 1;
        } else {
            first = static_cast<int>(std::floor(x1 - 0.5)) + 1;
            last = static_cast<int>(std::floor(x0 - 0.5));
        }
        first = std::max(first, clip_.left());
        last = std::min(last, clip_.right());
        const double he = h * len / std::abs(dx);
        const double slope = dy / dx;
        for (int c = first; c <= last; ++c) {
            const double yc = y0 + (c + 0.5 - x0) * slope;
            coverColumn(c, yc - he, yc + he);
        }
    }
}

void SpanRaster::thickPolyline(const float* xy, std::size_t n, float widthPx) {
    for (std::size_t i = 0; i + 1 < n; ++i) {
        thick(static_cast<double>(xy[2 * i]), static_cast<double>(xy[2 * i + 1]),
              static_cast<double>(xy[2 * i + 2]), static_cast<double>(xy[2 * i + 3]),
              static_cast<double>(widthPx));
    }
}

void SpanRaster::thickSegment(float x0, float y0, float x1, float y1, float widthPx) {
    thick(static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1),
          static_cast<double>(widthPx));
}

} // namespace esses::qt::detail
