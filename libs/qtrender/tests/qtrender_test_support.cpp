// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "qtrender_test_support.hpp"

#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace esses::qttest {

QString repoRoot() {
    return QString::fromUtf8(ESSES_TEST_REPO_ROOT);
}

QString artefactDir() {
    const QString dir = QString::fromUtf8(ESSES_TEST_BINARY_DIR) + QStringLiteral("/artefacts");
    QDir().mkpath(dir);
    return dir;
}

QString referencePath(const QString& name) {
    return QString::fromUtf8(ESSES_TEST_SOURCE_DIR) + QStringLiteral("/tests/reference/") + name +
           QStringLiteral(".png");
}

void prepareFontEnvironment() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", (repoRoot() + QStringLiteral("/tests/golden/fonts")).toLocal8Bit());
    }
}

int registerGoldenFonts() {
    static const int count = qt::SchemeFonts::registerApplicationFonts(
        std::filesystem::path(repoRoot().toStdWString()) / "tests" / "golden" / "fonts");
    return count;
}

project::Scheme testScheme(double sizeOffset) {
    project::Scheme s;
    s.name = "Qtrender Test";
    s.background = "light";
    const auto font = [&](double pt, int weight = 400, bool italic = false) {
        return project::FontSpec{"Noto Sans", pt + sizeOffset, weight, italic};
    };
    s.fonts["axis"] = font(8.0);
    s.fonts["axisRotated"] = font(8.5);
    s.fonts["caption"] = font(9.0, 600);
    s.fonts["key"] = font(8.25);
    s.fonts["small"] = font(7.0);
    s.fonts["gauge"] = font(14.0, 600);
    s.fonts["reportTable"] = font(9.5);
    s.fonts["equation"] = font(8.75, 400, true);
    s.fonts["colourBands"] = font(7.25);
    s.fonts["trackLabel"] = font(8.0, 600);
    s.fonts["valuesItem"] = font(9.0);
    s.roles["windowBackground"] = 0xF0F0F0FFU;
    s.roles["cptBackground"] = 0xFFFFFFFFU;
    s.roles["cptFrame"] = 0x3A3D41FFU;
    s.roles["cptForeground2"] = 0x606060FFU;
    s.channelPalette = {0xFF5252FFU, 0x40C4FFFFU, 0x69F0AEFFU};
    return s;
}

std::shared_ptr<const qt::SchemeFonts> testFonts(double sizeOffset) {
    return std::make_shared<const qt::SchemeFonts>(testScheme(sizeOffset));
}

plot::DrawList emptyList(int logicalW, int logicalH, float dpr) {
    plot::DrawList l;
    l.dpr = dpr;
    l.widthPx = static_cast<int>(std::lround(logicalW * static_cast<double>(dpr)));
    l.heightPx = static_cast<int>(std::lround(logicalH * static_cast<double>(dpr)));
    return l;
}

QImage rasterise(const plot::DrawList& list, plot::Rgba clear, std::shared_ptr<const qt::SchemeFonts> fonts) {
    qt::RasterBackend backend(fonts ? std::move(fonts) : testFonts(), nullptr);
    backend.beginLayer(plot::LayerKind::Static, list.widthPx, list.heightPx, list.dpr, clear);
    backend.draw(list);
    const plot::LayerHandle h = backend.endLayer();
    QImage img = backend.image(h);
    backend.release(h);
    return img;
}

namespace {

std::size_t addVertices(plot::DrawList& l, plot::CmdKind kind, const std::vector<float>& xy,
                        plot::Rgba colour, float widthPx = 1.0F, float pointSizePx = 0.0F) {
    plot::DrawCmd cmd;
    cmd.kind = kind;
    cmd.vertexOffset = static_cast<std::uint32_t>(l.verts.size() / 2);
    cmd.vertexCount = static_cast<std::uint32_t>(xy.size() / 2);
    cmd.colour = colour;
    cmd.widthPx = widthPx;
    cmd.pointSizePx = pointSizePx;
    l.verts.insert(l.verts.end(), xy.begin(), xy.end());
    l.cmds.push_back(cmd);
    return l.cmds.size() - 1;
}

} // namespace

plot::DrawList geometryScene(float dpr) {
    plot::DrawList l = emptyList(240, 160, dpr);
    const auto d = [dpr](double logical) { return static_cast<float>(logical * static_cast<double>(dpr)); };
    l.clear(0xFFFFFFFFU);
    // Grid: 1 logical px lines on pixel centres.
    for (int i = 1; i < 6; ++i) {
        const std::vector<float> v = {d(40.0 * i + 0.5), d(0.0), d(40.0 * i + 0.5), d(160.0)};
        l.polyline(v.data(), 2, 0xD0D0D0FFU, d(1.0));
    }
    for (int i = 1; i < 4; ++i) {
        const std::vector<float> h = {d(0.0), d(40.0 * i + 0.5), d(240.0), d(40.0 * i + 0.5)};
        l.polyline(h.data(), 2, 0xD0D0D0FFU, d(1.0));
    }
    // A translucent band.
    l.filledRect({d(0.0), d(60.0), d(240.0), d(20.0)}, 0x4C9AFF40U);
    // A 1 logical px sine trace and a 2 logical px dashed trace.
    std::vector<float> sine;
    std::vector<float> cosine;
    for (int i = 0; i <= 200; ++i) {
        const double x = 20.0 + i;
        sine.push_back(d(x));
        sine.push_back(d(80.0 + 50.0 * std::sin(i / 15.0)));
        cosine.push_back(d(x));
        cosine.push_back(d(80.0 + 40.0 * std::cos(i / 21.0)));
    }
    l.polyline(sine.data(), sine.size() / 2, 0xE53935FFU, d(1.0));
    l.polyline(cosine.data(), cosine.size() / 2, 0x1E88E5FFU, d(2.0), plot::DashId::Dash);
    // Points: small and round.
    std::vector<float> pts;
    for (int i = 0; i < 10; ++i) {
        pts.push_back(d(25.0 + 20.0 * i));
        pts.push_back(d(20.0));
    }
    addVertices(l, plot::CmdKind::Points, pts, 0x2E7D32FFU, 1.0F, d(4.0));
    // Lines: vertical segments.
    std::vector<float> segs;
    for (int i = 0; i < 8; ++i) {
        segs.push_back(d(30.0 + 25.0 * i + 0.5));
        segs.push_back(d(140.0));
        segs.push_back(d(30.0 + 25.0 * i + 0.5));
        segs.push_back(d(150.0 - i));
    }
    addVertices(l, plot::CmdKind::Lines, segs, 0x6A1B9AFFU, d(1.0));
    // Quads: one axis-aligned, one diamond.
    addVertices(l, plot::CmdKind::Quads,
                {d(200.0), d(10.0), d(230.0), d(10.0), d(230.0), d(30.0), d(200.0), d(30.0), // rectangle
                 d(215.0), d(110.0), d(230.0), d(125.0), d(215.0), d(140.0), d(200.0), d(125.0)},
                0xFFB000C0U);
    // Scissor: a rectangle clipped to the lower right, then the full layer again.
    l.scissor({d(120.0), d(100.0), d(60.0), d(30.0)});
    l.filledRect({d(100.0), d(90.0), d(120.0), d(60.0)}, 0x00897B80U);
    l.scissor({0.0F, 0.0F, 0.0F, 0.0F});
    l.filledRect({d(4.0), d(150.0), d(10.0), d(6.0)}, 0x000000FFU);
    return l;
}

QRgb pixel(const QImage& img, int x, int y) {
    const QRgb p = img.pixel(x, y); // raw (premultiplied) for Format_ARGB32_Premultiplied
    return img.format() == QImage::Format_ARGB32_Premultiplied ? qUnpremultiply(p) : p;
}

bool sameColour(QRgb a, QRgb b, int tolerance) {
    return std::abs(qRed(a) - qRed(b)) <= tolerance && std::abs(qGreen(a) - qGreen(b)) <= tolerance &&
           std::abs(qBlue(a) - qBlue(b)) <= tolerance && std::abs(qAlpha(a) - qAlpha(b)) <= tolerance;
}

QRect inkBounds(const QImage& img, QRgb background, int tolerance) {
    const QImage a = img.convertToFormat(QImage::Format_ARGB32);
    int x0 = a.width();
    int y0 = a.height();
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < a.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (!sameColour(row[x], background, tolerance)) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

void saveArtefact(const QImage& img, const QString& name) {
    img.save(artefactDir() + QLatin1Char('/') + name + QStringLiteral(".png"), "PNG");
}

bool matchesReference(const QImage& img, const QString& name, QString& message) {
    saveArtefact(img, name);
    const QString path = referencePath(name);
    if (qEnvironmentVariableIntValue("ESSES_UPDATE_REFERENCES") == 1) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!img.save(path, "PNG")) {
            message = QStringLiteral("cannot write ") + path;
            return false;
        }
        return true;
    }
    const QImage expected(path);
    if (expected.isNull()) {
        message = QStringLiteral("missing reference ") + path +
                  QStringLiteral(" (render it with ESSES_UPDATE_REFERENCES=1 after review)");
        return false;
    }
    const qt::ImageDiff diff = qt::compareImages(img, expected, qt::kGoldenChannelTolerance, true);
    if (!diff.passes()) {
        diff.diff.save(artefactDir() + QLatin1Char('/') + name + QStringLiteral(".diff.png"), "PNG");
        img.save(artefactDir() + QLatin1Char('/') + name + QStringLiteral(".actual.png"), "PNG");
        message = name + QStringLiteral(": ") + diff.summary();
        return false;
    }
    return true;
}

} // namespace esses::qttest
