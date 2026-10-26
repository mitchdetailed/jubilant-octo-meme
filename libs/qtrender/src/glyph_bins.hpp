// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal to esses_qtrender: one subpixel position count for every font engine that draws plot text (D-220).
// Qt's raster engine places a cached glyph at its device x position quantised into
// QFontEngine::m_subPixelPositionCount bins and caches one glyph image per bin. FreeType's engine (the
// offscreen platform on Windows, Linux) fixes the count at 4 when it is created. CoreText's (macOS) and
// DirectWrite's leave it 0, and Qt computes it at the engine's first glyph-cache fill
// (QTextureGlyphCache::populate): the number of distinct images of the fill's first glyph at twelve offsets,
// under that fill's transform, i.e. at the painter's DPR. Font engines belong to one thread (QFontCache is
// per thread; a worker keeps its engines for its life), so the count, and with it the glyph image a position
// picks, depended on what that thread drew first and at which DPR: on macOS a Static layer painted on a
// TaskPool worker differed from one pass on the GUI thread at DPR 1.5 and 2, and a graph moved from DPR 1 to
// 2 and back did not return to its pixels (D-217 (n), docs/milestones/M4.md section 10 item 51). Pinning the
// count before an engine fills a glyph cache makes the image a position picks depend on the position alone.
// Uses Qt private API (QFontEngine, QFontEngineMulti, QFontPrivate, QRawFontPrivate; Qt6::GuiPrivate),
// checked against the local 6.7.2 kit and the CI and release kit 6.12.0: a Qt version bump re-checks it
// (D-220).
#pragma once

#include <QFont>
#include <QString>

namespace esses::qt::detail {

// QFontEngineFT's fixed count, so the pinned engines quantise as the FreeType ones do.
inline constexpr int kGlyphSubpixelBins = 4;

// Whether drawText pins (process-wide). On by default on macOS only, where the plot text's engines are
// CoreText's; elsewhere the offscreen and Linux engines are FreeType's, already at 4, and the output of the
// Windows platform's DirectWrite engines is left as it is (D-220). Probes and tests switch it.
bool glyphBinPinning() noexcept;
void setGlyphBinPinning(bool on) noexcept;

// Called by drawText after QPainter::setFont and before drawStaticText, with the painter's own
// (device-resolved) font: gives every font engine of the calling thread that `text` can be drawn with, and
// that supports subpixel positions, kGlyphSubpixelBins bins. Every call: the font's Script_Common engine and
// the fallback engines it has loaded. When `newText` (QStaticText is about to lay `text` out) or when that
// found an engine not pinned yet (new, or recreated after a font cache clear): `text` is laid out first, as
// QStaticText lays it out, so the fallback and per-script engines it needs exist and are pinned too.
// Re-pinning an engine that has filled glyph caches is safe: the caches are keyed by glyph and bin. Nothing
// when pinning is off. Returns the number of engines whose count changed.
int pinGlyphSubpixelBins(const QFont& deviceFont, const QString& text, bool newText);

} // namespace esses::qt::detail
