// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "glyph_bins.hpp"

#include <QChar>
#include <QGlyphRun>
#include <QList>
#include <QRawFont>
#include <QTextLayout>
#include <QtGlobal>
#include <atomic>

// Qt private API (Qt6::GuiPrivate, D-220): the engines' subpixel position count.
#include <QtGui/private/qfont_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qrawfont_p.h>

namespace esses::qt::detail {

namespace {

#if defined(Q_OS_MACOS)
constexpr bool kPinByDefault = true;
#else
constexpr bool kPinByDefault = false;
#endif

std::atomic<bool> gPinning{kPinByDefault};

// Pins one engine, or the loaded engines of a QFontEngineMulti (its primary and the fallbacks it has loaded;
// a fallback not loaded yet is null). Engines that place glyphs at whole pixels only are left alone (Qt gives
// them one bin). Returns the number of engines changed.
int pinEngine(QFontEngine* engine) {
    if (engine == nullptr) {
        return 0;
    }
    if (engine->type() == QFontEngine::Multi) {
        const auto* multi = static_cast<const QFontEngineMulti*>(engine);
        int changed = 0;
        for (int i = 0; i <= multi->fallbackFamilyCount(); ++i) { // fallbackFamilyCount() + 1 engines
            changed += pinEngine(multi->engine(i));
        }
        return changed;
    }
    if (!engine->supportsSubPixelPositions() || engine->m_subPixelPositionCount == kGlyphSubpixelBins) {
        return 0;
    }
    engine->m_subPixelPositionCount = kGlyphSubpixelBins;
    return 1;
}

} // namespace

bool glyphBinPinning() noexcept {
    return gPinning.load(std::memory_order_relaxed);
}

void setGlyphBinPinning(bool on) noexcept {
    gPinning.store(on, std::memory_order_relaxed);
}

int pinGlyphSubpixelBins(const QFont& deviceFont, const QString& text, bool newText) {
    if (!glyphBinPinning()) {
        return 0;
    }
    // The engine QPainter::drawStaticText itself looks up (QFontPrivate::engineForScript(Script_Common)).
    int changed = pinEngine(QFontPrivate::get(deviceFont)->engineForScript(QChar::Script_Common));
    if (!newText && changed == 0) {
        return 0;
    }
    // Shaped as QStaticTextPrivate::paintText shapes it (one unbounded line in this font, on this thread's
    // font cache): loads the fallback and per-script engines the text needs; its glyph runs name them.
    QTextLayout layout(text, deviceFont);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    for (const QGlyphRun& run : runs) {
        const QRawFont raw = run.rawFont();
        changed += pinEngine(QRawFontPrivate::get(raw)->fontEngine);
    }
    return changed;
}

} // namespace esses::qt::detail
