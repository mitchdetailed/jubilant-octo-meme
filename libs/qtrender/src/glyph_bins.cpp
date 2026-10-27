// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "glyph_bins.hpp"

#include <QByteArrayAlgorithms>
#include <QChar>
#include <QGlyphRun>
#include <QList>
#include <QRawFont>
#include <QTextLayout>
#include <QtGlobal>
#include <atomic>
#include <climits>

// Qt private API (Qt6::GuiPrivate, D-220): the engines' subpixel position count, and QFIXED_MAX.
#include <QtGui/private/qfixed_p.h>
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

// Whether the Qt library loaded at run time is the version whose private headers this file was compiled
// with: only then are the private classes laid out as this file reads and writes them (D-220).
bool runtimeQtIsBuildQt() noexcept {
    static const bool same = qstrcmp(qVersion(), QT_VERSION_STR) == 0;
    return same;
}

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
    return gPinning.load(std::memory_order_relaxed) && runtimeQtIsBuildQt();
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
    // Laid out as QStaticTextPrivate::paintText lays it out (lines until createLine() gives none, each
    // QFIXED_MAX wide, in this font, on this thread's font cache), so text after a line separator (U+2028;
    // plot text is not sanitised) is shaped too: loads the fallback and per-script engines the text needs;
    // its glyph runs name them. The layout keeps its shaping (setCacheEnabled): glyphRuns() does not shape
    // the text again.
    QTextLayout layout(text, deviceFont);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(QFIXED_MAX);
    }
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    for (const QGlyphRun& run : runs) {
        const QRawFont raw = run.rawFont();
        changed += pinEngine(QRawFontPrivate::get(raw)->fontEngine);
    }
    return changed;
}

} // namespace esses::qt::detail
