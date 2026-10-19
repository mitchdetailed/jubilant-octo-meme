// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Header-only helpers of the M4 HiDPI tests (D-209; AC-M4-07; 04 section 6.6; 06 section 17): the widgets
// sweep (libs/widgets/tests/m4_hidpi/tst_dpr_sweep.cpp) and the shell's DPR switch
// (tests/ui/tst_dpr_switch.cpp, which includes this file by path, so nothing here depends on another test
// helper).
//   * Screens: the offscreen platform's screen configuration (its "configfile" platform argument) with four
//     screens side by side at 96, 120, 144 and 192 dpi over a base of 96, i.e. DPR 1, 1.25, 1.5 and 2 through
//     Qt's high-DPI scaling, the way the Windows platform reports a 100/125/150/200 % monitor (logical DPI,
//     platform DPR 1). Each is 1600 x 1000 logical pixels. A window moved from one to another changes screen
//     and DPR as a window dragged across monitors does (Qt sends ScreenChangeInternal and
//     DevicePixelRatioChange); unlike Windows, which keeps a window's logical size on a DPI change, the
//     offscreen platform keeps its native size once the move is reported, so a test that needs the logical
//     size kept sets it again (tests/ui/tst_dpr_switch.cpp moveTo). A display whose scale changes in place
//     reaches the same DevicePixelRatioChange, but the offscreen platform cannot change a screen's DPI at run
//     time through public API.
//   * Crispness of a DrawList as RasterBackend draws it (04 section 6.6 item 4: sizes are rounded to device
//     pixels where the commands are emitted; D-209): its axis-aligned opaque geometry (Clear, Scissor, opaque
//     FilledRect and axis-aligned Quads, opaque solid Lines and Polylines made only of horizontal and
//     vertical segments) rasterised alone covers whole pixels, i.e. every pixel of that picture has the
//     colour of one of those commands or is untouched; a stroke or a rectangle straddling a pixel edge leaves
//     a partially covered pixel of a blended colour. Slanted strokes, translucent colours, dashes and text
//     are antialiased by design and left out.
//   * Image comparison: two images equal pixel for pixel after conversion to one format.
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/project/rgba.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace esses::widgets::dprtest {

struct ScreenSpec {
    const char* name;
    int nativeX;    // native origin; the logical origin is the same point (Qt keeps a screen's origin)
    int logicalDpi; // over a base of 96
    qreal dpr;
};

inline constexpr std::array<ScreenSpec, 4> kScreens{{
    {"s100", 0, 96, 1.0},
    {"s125", 2000, 120, 1.25},
    {"s150", 4500, 144, 1.5},
    {"s200", 7500, 192, 2.0},
}};
inline constexpr int kScreenLogicalW = 1600;
inline constexpr int kScreenLogicalH = 1000;

// initMain (before the QApplication exists): writes the configuration into the test's binary directory and
// selects the offscreen platform with it. The platform argument list is ':'-separated, so the file is named
// relative to the working directory (a Windows drive letter would split the argument).
inline void useSweepScreens(const std::string& testName, const std::filesystem::path& binaryDir) {
    std::filesystem::current_path(binaryDir);
    const std::filesystem::path config = testName + ".screens.json";
    std::ofstream out(config, std::ios::binary | std::ios::trunc);
    out << R"({"synchronousWindowSystemEvents": false, "windowFrameMargins": false, "screens": [)";
    bool first = true;
    for (const ScreenSpec& s : kScreens) {
        const qreal f = static_cast<qreal>(s.logicalDpi) / 96.0;
        out << (first ? "" : ",") << R"({"name": ")" << s.name << R"(", "x": )" << s.nativeX
            << R"(, "y": 0, "width": )" << std::lround(kScreenLogicalW * f) << R"(, "height": )"
            << std::lround(kScreenLogicalH * f) << R"(, "logicalDpi": )" << s.logicalDpi
            << R"(, "logicalBaseDpi": 96, "dpr": 1.0})";
        first = false;
    }
    out << "]}";
    out.close();
    qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + config.generic_string()).c_str());
}

inline QScreen* screenNamed(const QString& name) {
    for (QScreen* s : QGuiApplication::screens()) {
        if (s->name() == name) {
            return s;
        }
    }
    return nullptr;
}

inline const ScreenSpec* specForDpr(qreal dpr) {
    for (const ScreenSpec& s : kScreens) {
        if (std::abs(s.dpr - dpr) < 1e-9) {
            return &s;
        }
    }
    return nullptr;
}

inline QScreen* screenForDpr(qreal dpr) {
    const ScreenSpec* s = specForDpr(dpr);
    return s == nullptr ? nullptr : screenNamed(QString::fromLatin1(s->name));
}

// A logical point `offset` inside the screen of `dpr` (its logical origin is its native one).
inline QPoint pointOn(qreal dpr, QPoint offset) {
    const ScreenSpec* s = specForDpr(dpr);
    return s == nullptr ? offset : QPoint(s->nativeX, 0) + offset;
}

inline QString dprText(qreal dpr) {
    return QString::number(dpr, 'g', 4);
}

inline QRgb premultipliedOf(plot::Rgba c) {
    return qPremultiply(qRgba(static_cast<int>(project::red(c)), static_cast<int>(project::green(c)),
                              static_cast<int>(project::blue(c)), static_cast<int>(project::alpha(c))));
}

inline bool opaque(plot::Rgba c) {
    return project::alpha(c) == 0xFFU;
}

// Whether the vertices [first, first + count) of `list` make at least one segment and every segment is
// horizontal or vertical (`pairs`: independent segments, as Lines; else consecutive vertices, as Polyline;
// non-finite vertices are gaps; a dot, only zero-length segments, is round by design).
inline bool axisAligned(const plot::DrawList& list, std::size_t first, std::size_t count, bool pairs) {
    const float* v = list.verts.data() + 2 * first;
    const std::size_t step = pairs ? 2 : 1;
    bool segment = false;
    for (std::size_t i = 0; i + 1 < count; i += step) {
        const bool finite = std::isfinite(v[2 * i]) && std::isfinite(v[2 * i + 1]) &&
                            std::isfinite(v[2 * i + 2]) && std::isfinite(v[2 * i + 3]);
        if (!finite) {
            continue;
        }
        const bool sameX = v[2 * i] == v[2 * i + 2];
        const bool sameY = v[2 * i + 1] == v[2 * i + 3];
        if (!sameX && !sameY) {
            return false;
        }
        segment = segment || !(sameX && sameY);
    }
    return segment;
}

// The check above. `staticLayer`: rasterised as LayerCache does a Static list (RasterBackend::
// rasteriseStatic, the span rasteriser); else painted as a widget paints a Dynamic or Chrome list
// (RasterBackend::paintList on a DPR-tagged image). Up to five problems, each naming the device pixel and
// the colour found.
inline QStringList crispnessProblems(const plot::DrawList& list, const QString& what, bool staticLayer) {
    QStringList out;
    if (list.widthPx <= 0 || list.heightPx <= 0) {
        return out;
    }
    plot::DrawList kept;
    kept.widthPx = list.widthPx;
    kept.heightPx = list.heightPx;
    kept.dpr = list.dpr;
    std::vector<QRgb> colours{0U};
    const auto keep = [&](const plot::DrawCmd& c) {
        plot::DrawCmd copy = c;
        if (c.kind == plot::CmdKind::Lines || c.kind == plot::CmdKind::Polyline ||
            c.kind == plot::CmdKind::Quads) {
            const auto first = static_cast<std::ptrdiff_t>(c.vertexOffset);
            const auto count = static_cast<std::ptrdiff_t>(c.vertexCount);
            copy.vertexOffset = static_cast<std::uint32_t>(kept.verts.size() / 2);
            kept.verts.insert(kept.verts.end(), list.verts.begin() + 2 * first,
                              list.verts.begin() + 2 * (first + count));
        }
        kept.cmds.push_back(copy);
        if (c.kind != plot::CmdKind::Scissor) {
            colours.push_back(premultipliedOf(c.colour));
        }
    };
    for (const plot::DrawCmd& c : list.cmds) {
        if ((static_cast<std::size_t>(c.vertexOffset) + c.vertexCount) * 2 > list.verts.size()) {
            continue;
        }
        switch (c.kind) {
        case plot::CmdKind::Clear:
        case plot::CmdKind::Scissor:
            keep(c);
            break;
        case plot::CmdKind::FilledRect:
            if (opaque(c.colour)) {
                keep(c);
            }
            break;
        case plot::CmdKind::Quads: {
            bool aligned = opaque(c.colour);
            const float* v = list.verts.data() + 2 * static_cast<std::size_t>(c.vertexOffset);
            for (std::size_t q = 0; aligned && q + 3 < c.vertexCount; q += 4) {
                for (std::size_t k = 0; k < 4; ++k) {
                    const std::size_t a = q + k;
                    const std::size_t b = q + (k + 1) % 4;
                    aligned = aligned && (v[2 * a] == v[2 * b] || v[2 * a + 1] == v[2 * b + 1]);
                }
            }
            if (aligned) {
                keep(c);
            }
            break;
        }
        case plot::CmdKind::Lines:
        case plot::CmdKind::Polyline:
            if (opaque(c.colour) && c.dash == plot::DashId::Solid &&
                axisAligned(list, c.vertexOffset, c.vertexCount, c.kind == plot::CmdKind::Lines)) {
                keep(c);
            }
            break;
        default:
            break;
        }
    }
    const qt::SchemeFonts fonts;
    QImage img;
    if (staticLayer) {
        img = qt::RasterBackend::rasteriseStatic(kept, fonts);
    } else {
        img = QImage(kept.widthPx, kept.heightPx, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(static_cast<qreal>(kept.dpr));
        img.fill(Qt::transparent);
        QPainter p(&img);
        qt::RasterBackend::paintList(p, kept, fonts);
    }
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.height() && out.size() < 5; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width() && out.size() < 5; ++x) {
            if (std::find(colours.begin(), colours.end(), row[x]) == colours.end()) {
                out << QStringLiteral("%1: pixel (%2, %3) is #%4, a partial cover")
                           .arg(what)
                           .arg(x)
                           .arg(y)
                           .arg(row[x], 8, 16, QLatin1Char('0'));
            }
        }
    }
    return out;
}

inline QImage rgb32(const QImage& img) {
    return img.convertToFormat(QImage::Format_RGB32);
}

// Pixels that differ (RGB) between two images of the same size; -1 when the sizes differ.
inline long long differingPixels(const QImage& a, const QImage& b) {
    if (a.size() != b.size()) {
        return -1;
    }
    const QImage x = rgb32(a);
    const QImage y = rgb32(b);
    long long n = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            if ((p[col] & 0x00FFFFFFU) != (q[col] & 0x00FFFFFFU)) {
                ++n;
            }
        }
    }
    return n;
}

// As differingPixels, leaving out the pixels inside `masked` (device rectangles in the images' coordinates).
inline long long differingPixelsOutside(const QImage& a, const QImage& b, const QList<QRect>& masked) {
    if (a.size() != b.size()) {
        return -1;
    }
    const QImage x = rgb32(a);
    const QImage y = rgb32(b);
    long long n = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            if ((p[col] & 0x00FFFFFFU) == (q[col] & 0x00FFFFFFU)) {
                continue;
            }
            const QPoint at(col, row);
            if (std::none_of(masked.begin(), masked.end(), [&](const QRect& r) { return r.contains(at); })) {
                ++n;
            }
        }
    }
    return n;
}

// The device rectangle Qt paints a widget into: its logical rectangle (in its window) scaled by the DPR,
// origin and size rounded each (QTransform::mapRect of a scale; the raster engine rounds a 1:1 image blit's
// origin the same way). Rounded separately, two adjacent widgets' rectangles can share their boundary
// column or row at a fractional DPR; the one painted later shows there.
inline QRect deviceRectOf(const QRect& logicalInWindow, qreal dpr) {
    return QRect(static_cast<int>(std::lround(logicalInWindow.x() * dpr)),
                 static_cast<int>(std::lround(logicalInWindow.y() * dpr)),
                 static_cast<int>(std::lround(logicalInWindow.width() * dpr)),
                 static_cast<int>(std::lround(logicalInWindow.height() * dpr)));
}

} // namespace esses::widgets::dprtest
