// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Pixel comparison for golden images and render parity (04 section 6.7 item 4, 08 section 6.4, D-127;
// AC-M1-04, AC-M1-05): a pixel differs when any channel (straight-alpha r, g, b, a) differs by more than
// 8/255; an image fails when more than 0.5 % of its pixels differ, or when the sizes differ. The device pixel
// ratio tags are not compared (the pixel sizes are). The optional diff image (for the CI artefact) shows
// differing pixels in opaque red over a faded grey copy of the expected image.
#pragma once

#include <QImage>
#include <QString>
#include <cstdint>

namespace esses::qt {

inline constexpr int kGoldenChannelTolerance = 8;       // per channel, out of 255
inline constexpr double kGoldenMaxDiffFraction = 0.005; // 0.5 % of the pixels

struct ImageDiff {
    bool sizeMatches = false;
    std::int64_t totalPixels = 0;
    std::int64_t differingPixels = 0;
    int maxChannelDelta = 0; // over all pixels and channels
    QImage diff;             // only when requested

    // differingPixels / totalPixels; 1 when the sizes differ or the images are empty.
    double fraction() const noexcept;
    // Size match and at most `maxFraction` of the pixels differing.
    bool passes(double maxFraction = kGoldenMaxDiffFraction) const noexcept;
    // One line for a test failure message: "3 of 20000 pixels (0.015 %) differ, max channel delta 40".
    QString summary() const;
};

ImageDiff compareImages(const QImage& actual, const QImage& expected,
                        int channelTolerance = kGoldenChannelTolerance, bool makeDiffImage = false);

} // namespace esses::qt
