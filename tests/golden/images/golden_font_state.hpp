// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// For the repeatableInAnyOrder checks of the pixel golden tests (04 section 6.7 item 1: the same inputs give
// the same pixels whatever the process rendered before). The calling thread's font state carries over from
// one render to the next, but rendering the cases again mostly reuses glyphs Qt has already cached, so it
// hardly changes; perturbFontState draws what other text of a process may: every golden font file, vertical
// hinting only (subpixel glyph positions, as fontconfig's default hintslight gives every font on Linux), a
// size no other text uses (each glyph rasterised anew), the last glyph on a `round`-dependent quarter pixel.
// Text that Qt draws as outlines once depended on exactly that (libs/qtrender/src/raster_backend.cpp,
// drawText).
#pragma once

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QString>

namespace esses::goldentest {

inline void perturbFontState(int round) {
    QImage scratch(240, 64, QImage::Format_ARGB32_Premultiplied);
    scratch.fill(Qt::white);
    QPainter p(&scratch);
    p.setPen(Qt::black);
    for (const QFont::Weight weight : {QFont::Normal, QFont::Bold}) { // NotoSans-Regular and NotoSans-Bold
        QFont f(QStringLiteral("Noto Sans"));
        f.setPointSizeF(6.0 + 0.05 * round);
        f.setWeight(weight);
        f.setHintingPreference(QFont::PreferVerticalHinting);
        f.setStyleStrategy(QFont::NoSubpixelAntialias);
        p.setFont(f);
        p.drawText(QPointF(4.1 + 0.25 * (round % 4), 40.1 + 0.25 * (round / 4 % 4)),
                   QStringLiteral("-80.5%"));
    }
}

} // namespace esses::goldentest
