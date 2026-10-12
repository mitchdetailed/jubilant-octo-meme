// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtrender/image_diff.hpp>

#include <QColor>
#include <algorithm>
#include <cstdlib>

namespace esses::qt {

double ImageDiff::fraction() const noexcept {
    if (!sizeMatches || totalPixels <= 0) {
        return 1.0;
    }
    return static_cast<double>(differingPixels) / static_cast<double>(totalPixels);
}

bool ImageDiff::passes(double maxFraction) const noexcept {
    return sizeMatches && totalPixels > 0 &&
           static_cast<double>(differingPixels) <= maxFraction * static_cast<double>(totalPixels);
}

QString ImageDiff::summary() const {
    if (!sizeMatches) {
        return QStringLiteral("image sizes differ");
    }
    return QStringLiteral("%1 of %2 pixels (%3 %) differ, max channel delta %4")
        .arg(differingPixels)
        .arg(totalPixels)
        .arg(fraction() * 100.0, 0, 'f', 3)
        .arg(maxChannelDelta);
}

ImageDiff compareImages(const QImage& actual, const QImage& expected, int channelTolerance,
                        bool makeDiffImage) {
    ImageDiff d;
    if (actual.isNull() || expected.isNull() || actual.size() != expected.size()) {
        return d;
    }
    d.sizeMatches = true;
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32);
    const QImage e = expected.convertToFormat(QImage::Format_ARGB32);
    const int w = a.width();
    const int h = a.height();
    d.totalPixels = static_cast<std::int64_t>(w) * h;
    if (makeDiffImage) {
        d.diff = QImage(w, h, QImage::Format_ARGB32);
    }
    for (int y = 0; y < h; ++y) {
        const auto* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* re = reinterpret_cast<const QRgb*>(e.constScanLine(y));
        auto* rd = makeDiffImage ? reinterpret_cast<QRgb*>(d.diff.scanLine(y)) : nullptr;
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pe = re[x];
            const int delta = std::max({std::abs(qRed(pa) - qRed(pe)), std::abs(qGreen(pa) - qGreen(pe)),
                                        std::abs(qBlue(pa) - qBlue(pe)), std::abs(qAlpha(pa) - qAlpha(pe))});
            d.maxChannelDelta = std::max(d.maxChannelDelta, delta);
            const bool differs = delta > channelTolerance;
            if (differs) {
                ++d.differingPixels;
            }
            if (rd != nullptr) {
                const int grey = 192 + qGray(pe) / 4;
                rd[x] = differs ? qRgb(255, 0, 0) : qRgb(grey, grey, grey);
            }
        }
    }
    return d;
}

} // namespace esses::qt
