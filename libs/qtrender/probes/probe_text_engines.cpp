// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Diagnostic probe (ESSES_BUILD_PROBES; ctest qtrender.probe_text_engines and its platform entries, label
// probe; D-220): why esses_qtrender's text differs between threads and DPR histories on macOS while lines and
// fills match (D-217 (n), docs/milestones/M4.md section 10 item 51), and whether pinning the font engines'
// subpixel position count (src/glyph_bins.hpp) removes the difference. Every finding is one line starting
// with "PROBE ", so `grep PROBE` of the ctest log is the report. "PROBE env": the Qt version, the platform,
// whether pinning is on by default, Qt's FreeType, the application font. "PROBE font": per scheme role and
// hinting, the engine type that draws it, its pixel size, glyph format, whether it places glyphs at subpixel
// positions or keeps its own glyph cache (FreeType), its bin count and face. "PROBE emulate": the bin count
// Qt would compute at an engine's first glyph-cache fill (Qt's own calculateSubPixelPositionCount) per glyph
// and scale, and where the glyph's image changes over 48 offsets of a pixel. "PROBE history": fresh threads
// that draw one scene after different DPR histories (2 then 1, 1 alone, 1.5 then 1, 1 then 2, 2 alone), each
// role's bins after each step and the differing pixels of images that should be equal, pinning off and on;
// with pinning on also how many pixels pinning moves against Qt's own count. "PROBE strips": a Static layer
// in strips on a fresh TaskPool's workers against one pass on a thread that drew at DPR 1 first
// (qtrender.tst_static_strips' failing shape). "PROBE recreate": a thread's font cache cleared (what a font
// database change does) between DPR 1 and DPR 2 draws. "PROBE layout": one run's glyphs and positions, and
// each role's face, on the GUI thread and a worker. "PROBE log": the qt.qpa.fonts messages that bear on the
// hypotheses (with their thread) and the fontDatabaseChanged signals. "PROBE verdict": the summary.
// The probe fails only when pinning is on and an image still depends on its thread's history, or an engine
// that places glyphs at subpixel positions is not at 4 bins: the fix's acceptance. With pinning off it
// reports. Images that differ are written to <build>/libs/qtrender/artefacts/probe_*.png.

#include "glyph_bins.hpp"
#include "qtrender_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QLoggingCategory>
#include <QPainter>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTextLayout>
#include <QThread>
#include <QTransform>
#include <QtGlobal>
#include <QtGui/qtguiglobal.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Qt private API (Qt6::GuiPrivate): the engines and Qt's own bin count.
#include <QtGui/private/qfixed_p.h>
#include <QtGui/private/qfont_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qtextureglyphcache_p.h>

using namespace esses;

namespace {

using Fonts = std::shared_ptr<const qt::SchemeFonts>;

void report(const QString& line) {
    qInfo("PROBE %s", qPrintable(line));
}

// ---- what the verdict is read from (test thread only) ----

struct Tally {
    QString engine;               // the primary engine type of the caption role
    bool subpixel = false;        // it places glyphs at subpixel positions
    std::set<int> binsWithoutPin; // the bins of every role's engine after a draw, pinning off
    std::int64_t diffWithoutPin = 0;
    std::int64_t diffWithPin = 0;
    int binsNot4WithPin = 0; // engines that place glyphs at subpixel positions, pinning on, not at 4
    int historyRowsWithBinsSpread = 0; // history rows (pinning off) whose threads ended with different bins
    bool recreated = false;            // an engine was recreated after the font cache was cleared
    bool layoutIdentical = true;       // glyphs, positions and faces
    int fontDatabaseChanges = 0;
    int invalidations = 0; // "Invalidating font database" / "Fonts have changed"
};

Tally& tally() {
    static Tally t;
    return t;
}

// ---- the qt.qpa.fonts messages, with the thread that logged them ----

std::mutex gLogMutex;
QStringList gFontLog;
QtMessageHandler gPreviousHandler = nullptr;

void fontLogHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QString category = QString::fromLatin1(context.category != nullptr ? context.category : "");
    if (category.startsWith(QStringLiteral("qt.qpa.fonts")) ||
        category.startsWith(QStringLiteral("qt.text.font"))) {
        const bool gui = QCoreApplication::instance() != nullptr &&
                         QThread::currentThread() == QCoreApplication::instance()->thread();
        const std::scoped_lock lock(gLogMutex);
        gFontLog.append(QStringLiteral("%1 thread=%2 %3: %4")
                            .arg(category, gui ? QStringLiteral("gui") : QStringLiteral("worker"),
                                 type == QtDebugMsg ? QStringLiteral("debug") : QStringLiteral("warning"),
                                 message.left(240)));
        if (type == QtDebugMsg) {
            return; // debug lines only in the report
        }
    }
    if (gPreviousHandler != nullptr) {
        gPreviousHandler(type, context, message);
    }
}

// ---- engines ----

QString typeName(int type) {
    switch (type) {
    case QFontEngine::Box:
        return QStringLiteral("Box");
    case QFontEngine::Multi:
        return QStringLiteral("Multi");
    case QFontEngine::Win:
        return QStringLiteral("GDI");
    case QFontEngine::Mac:
        return QStringLiteral("CoreText");
    case QFontEngine::Freetype:
        return QStringLiteral("FreeType");
    case QFontEngine::DirectWrite:
        return QStringLiteral("DirectWrite");
    default:
        return QStringLiteral("type%1").arg(type);
    }
}

QString formatName(int format) {
    switch (format) {
    case QFontEngine::Format_None:
        return QStringLiteral("None");
    case QFontEngine::Format_Mono:
        return QStringLiteral("Mono");
    case QFontEngine::Format_A8:
        return QStringLiteral("A8");
    case QFontEngine::Format_A32:
        return QStringLiteral("A32");
    case QFontEngine::Format_ARGB:
        return QStringLiteral("ARGB");
    default:
        return QStringLiteral("format%1").arg(format);
    }
}

QString hintName(int hinting) {
    switch (hinting) {
    case QFont::PreferDefaultHinting:
        return QStringLiteral("default");
    case QFont::PreferNoHinting:
        return QStringLiteral("none");
    case QFont::PreferVerticalHinting:
        return QStringLiteral("vertical");
    case QFont::PreferFullHinting:
        return QStringLiteral("full");
    default:
        return QStringLiteral("hint%1").arg(hinting);
    }
}

// The Script_Common engine the calling thread draws `logical` with on a QImage (the font resolved as
// QPainter::setFont resolves it, against the application font, at the image's DPI), and its primary engine.
struct Engines {
    QFontEngine* script = nullptr;
    QFontEngine* primary = nullptr;
};

Engines enginesOf(const QFont& logical) {
    QImage device(8, 8, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&device);
    p.setFont(logical);
    Engines e;
    e.script = QFontPrivate::get(p.font())->engineForScript(QChar::Script_Common);
    e.primary = e.script;
    if (e.script != nullptr && e.script->type() == QFontEngine::Multi) {
        e.primary = static_cast<QFontEngineMulti*>(e.script)->engine(0);
    }
    return e;
}

QString faceOf(const QFontEngine* engine) {
    const QByteArray file = engine->faceId().filename;
    return file.isEmpty() ? QStringLiteral("-") : QFileInfo(QString::fromUtf8(file)).fileName();
}

// "caption=4" and the loaded fallbacks' bins ("caption=4+[4,4]").
QString binsOf(const QString& role, const Engines& e) {
    if (e.primary == nullptr) {
        return role + QStringLiteral("=none");
    }
    QString s = role + QLatin1Char('=') + QString::number(e.primary->m_subPixelPositionCount);
    if (e.script->type() == QFontEngine::Multi) {
        const auto* multi = static_cast<const QFontEngineMulti*>(e.script);
        QStringList fallbacks;
        for (int i = 1; i <= multi->fallbackFamilyCount(); ++i) {
            if (const QFontEngine* f = multi->engine(i); f != nullptr && f != e.primary) {
                fallbacks.append(typeName(f->type()) + QLatin1Char(':') +
                                 QString::number(f->m_subPixelPositionCount));
            }
        }
        if (!fallbacks.isEmpty()) {
            s += QStringLiteral("+[") + fallbacks.join(QLatin1Char(',')) + QLatin1Char(']');
        }
    }
    return s;
}

struct Role {
    const char* name;
    plot::TextStyleId style;
};
constexpr Role kRoles[] = {{"axis", plot::TextStyleId::Axis},
                           {"caption", plot::TextStyleId::Caption},
                           {"key", plot::TextStyleId::Key}};

// The bins of every role's engines on the calling thread, and how many engines that place glyphs at subpixel
// positions are not at 4 (`collect`: their bins).
QString binsOfRoles(const qt::SchemeFonts& fonts, int* not4 = nullptr, std::set<int>* collect = nullptr) {
    QStringList parts;
    for (const Role& r : kRoles) {
        const Engines e = enginesOf(fonts.font(r.style));
        parts.append(binsOf(QString::fromLatin1(r.name), e));
        if (e.primary != nullptr && e.primary->supportsSubPixelPositions()) {
            if (not4 != nullptr && e.primary->m_subPixelPositionCount != qt::detail::kGlyphSubpixelBins) {
                ++*not4;
            }
            if (collect != nullptr) {
                collect->insert(e.primary->m_subPixelPositionCount);
            }
        }
    }
    return parts.join(QLatin1Char(' '));
}

// ---- Qt's own bin count ----

// QTextureGlyphCache::calculateSubPixelPositionCount, as populate() calls it at an engine's first fill.
class BinCounter : public QImageTextureGlyphCache {
public:
    BinCounter(QFontEngine::GlyphFormat format, const QTransform& transform)
        : QImageTextureGlyphCache(format, transform) {}
    int binCount(QFontEngine* engine, glyph_t glyph) {
        m_current_fontengine = engine;
        return calculateSubPixelPositionCount(glyph);
    }
    QImage mapAt(QFontEngine* engine, glyph_t glyph, qreal x) {
        m_current_fontengine = engine;
        return textureMapForGlyph(glyph, QFixedPoint(QFixed::fromReal(x), QFixed(0)));
    }
};

// ---- scenes and threads ----

// Text at many subpixel offsets: rows of runs in four styles stepping by 1/7 device px per row, and single
// glyphs with overhangs (j, f, /) swept across the width in steps under a pixel.
plot::DrawList probeScene(float dpr) {
    plot::DrawList l = qttest::emptyList(512, 320, dpr);
    l.clear(0xFFFFFFFFU);
    const auto d = [dpr](double logical) { return static_cast<float>(logical * static_cast<double>(dpr)); };
    const plot::TextStyleId styles[] = {plot::TextStyleId::Axis, plot::TextStyleId::Caption,
                                        plot::TextStyleId::Key, plot::TextStyleId::KeyValue};
    const char* const texts[] = {"Corr Speed 123.4 km/h", "Lap 2  -0:01.375  Throttle %", "jounce Diff"};
    for (int row = 0; row < 7; ++row) {
        const double y = 4.0 + 14.0 * row;
        for (int col = 0; col < 4; ++col) {
            const double x = 6.0 + 125.37 * col + row / (7.0 * static_cast<double>(dpr));
            l.textRun(texts[(row + col) % 3], d(x), d(y), styles[(row + col) % 4], plot::TextAnchor::TopLeft,
                      0x202124FFU);
        }
    }
    for (int k = 0; k < 540; ++k) {
        const double x = 0.05 + 0.93 * k;
        const double y = 104.0 + 14.0 * (k % 5);
        l.textRun("j", d(x), d(y), plot::TextStyleId::Caption, plot::TextAnchor::TopLeft, 0x202124FFU);
        l.textRun("f", d(x), d(y + 72.0), plot::TextStyleId::Axis, plot::TextAnchor::TopRight, 0x202124FFU);
        l.textRun("/", d(x), d(y + 144.0), plot::TextStyleId::Key, plot::TextAnchor::TopRight, 0x202124FFU);
    }
    return l;
}

// One pass on the calling thread (not a TaskPool worker: one strip).
QImage onePass(const plot::DrawList& list, const qt::SchemeFonts& fonts) {
    return qt::RasterBackend::rasteriseStatic(list, fonts, 0xFFFFFFFFU);
}

// Runs `fn` on a thread of its own: its font cache, and so its font engines, start empty.
template <typename Fn> void onFreshThread(Fn&& fn) {
    std::thread t(std::forward<Fn>(fn));
    t.join();
}

QString dprList(const std::vector<float>& dprs) {
    QStringList s;
    for (const float dpr : dprs) {
        s.append(QString::number(static_cast<double>(dpr)));
    }
    return s.join(QStringLiteral(","));
}

std::int64_t differing(const QImage& a, const QImage& b, const QString& name) {
    const qt::ImageDiff d = qt::compareImages(a, b, 0, true);
    const std::int64_t n = d.sizeMatches ? d.differingPixels : -1;
    if (n != 0) {
        qttest::saveArtefact(a, QStringLiteral("probe_%1.a").arg(name));
        qttest::saveArtefact(b, QStringLiteral("probe_%1.b").arg(name));
        if (!d.diff.isNull()) {
            qttest::saveArtefact(d.diff, QStringLiteral("probe_%1.diff").arg(name));
        }
    }
    return n;
}

// The application font's hinting preference (which the plot fonts, setting none, inherit) and the pinning
// switch, for one scope.
class Conditions {
public:
    Conditions(bool noHinting, bool pin)
        : savedFont_(QGuiApplication::font()), savedPin_(qt::detail::glyphBinPinning()) {
        QFont f = savedFont_;
        f.setHintingPreference(noHinting ? QFont::PreferNoHinting : QFont::PreferDefaultHinting);
        QGuiApplication::setFont(f);
        qt::detail::setGlyphBinPinning(pin);
    }
    ~Conditions() {
        qt::detail::setGlyphBinPinning(savedPin_);
        QGuiApplication::setFont(savedFont_);
    }
    Conditions(const Conditions&) = delete;
    Conditions& operator=(const Conditions&) = delete;

private:
    QFont savedFont_;
    bool savedPin_;
};

void addRows() {
    QTest::addColumn<bool>("noHinting");
    QTest::addColumn<bool>("pin");
    QTest::newRow("hint=default pin=off") << false << false;
    QTest::newRow("hint=default pin=on") << false << true;
    QTest::newRow("hint=none pin=off") << true << false;
    QTest::newRow("hint=none pin=on") << true << true;
}

QString rowName(bool noHinting, bool pin) {
    return QStringLiteral("hint=%1 pin=%2")
        .arg(noHinting ? QStringLiteral("none") : QStringLiteral("default"),
             pin ? QStringLiteral("on") : QStringLiteral("off"));
}

QString fileTag(bool noHinting, bool pin) {
    return QStringLiteral("%1_%2").arg(noHinting ? QStringLiteral("nohint") : QStringLiteral("hint"),
                                       pin ? QStringLiteral("pin") : QStringLiteral("nopin"));
}

// Adds one comparison's differing pixels to the tally of its pinning state.
void tallyDiff(bool pin, std::int64_t diff) {
    if (pin) {
        tally().diffWithPin += std::max<std::int64_t>(diff, 0);
    } else {
        tally().diffWithoutPin += std::max<std::int64_t>(diff, 0);
    }
}

} // namespace

class ProbeTextEngines : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        qttest::prepareFontEnvironment();
        // The font database's own account (CoreText: aliases, invalidation, smoothing), into the report.
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.fonts.debug=true"));
    }

private Q_SLOTS:
    void initTestCase() {
        gPreviousHandler = qInstallMessageHandler(&fontLogHandler);
        const int files = qttest::registerGoldenFonts();
        // Changes after the golden fonts' own registration (which emits it once per file).
        connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, [] { ++tally().fontDatabaseChanges; });
        const QFont app = QGuiApplication::font();
#if QT_CONFIG(freetype)
        const QString freetype = QStringLiteral("yes");
#else
        const QString freetype = QStringLiteral("no");
#endif
        report(QStringLiteral("env qt=%1 platform=%2 pinByDefault=%3 freetypeInKit=%4 goldenFonts=%5 "
                              "appFont=\"%6\" %7pt hint=%8")
                   .arg(QString::fromLatin1(qVersion()), QGuiApplication::platformName(),
                        qt::detail::glyphBinPinning() ? QStringLiteral("on") : QStringLiteral("off"),
                        freetype)
                   .arg(files)
                   .arg(app.family())
                   .arg(app.pointSizeF())
                   .arg(hintName(app.hintingPreference())));
    }

    void cleanupTestCase() {
        qInstallMessageHandler(gPreviousHandler);
        QStringList log;
        {
            const std::scoped_lock lock(gLogMutex);
            log = gFontLog;
        }
        for (const QString& line : std::as_const(log)) {
            if (line.contains(QStringLiteral("Invalidating font database")) ||
                line.contains(QStringLiteral("Fonts have changed"))) {
                ++tally().invalidations;
            }
        }
        // The lines that bear on the hypotheses (not the database's per-family registrations), at most 60.
        constexpr qsizetype kMaxLines = 60;
        const QStringList keys = {QStringLiteral("smoothing"),  QStringLiteral("alias"),
                                  QStringLiteral("Invalidat"),  QStringLiteral("changed"),
                                  QStringLiteral("Populating"), QStringLiteral("createEngine"),
                                  QStringLiteral("allback"),    QStringLiteral("warning:")};
        const auto bearsOnHypotheses = [&keys](const QString& line) {
            const auto inLine = [&line](const QString& k) { return line.contains(k, Qt::CaseInsensitive); };
            return std::any_of(keys.begin(), keys.end(), inLine);
        };
        // Each distinct line once, in the order first logged, with how often it was logged.
        QStringList distinct;
        std::map<QString, int> times;
        for (const QString& line : std::as_const(log)) {
            if (bearsOnHypotheses(line) && times[line]++ == 0) {
                distinct.append(line);
            }
        }
        qsizetype shown = 0;
        for (const QString& line : std::as_const(distinct)) {
            if (shown++ < kMaxLines) {
                report(QStringLiteral("log x%1 %2").arg(times[line]).arg(line));
            }
        }
        report(QStringLiteral("log lines=%1 (%2 distinct shown) fontDatabaseChanged=%3 invalidations=%4")
                   .arg(log.size())
                   .arg(std::min(shown, kMaxLines))
                   .arg(tally().fontDatabaseChanges)
                   .arg(tally().invalidations));

        const Tally& t = tally();
        QStringList bins;
        for (const int b : t.binsWithoutPin) {
            bins.append(QString::number(b));
        }
        report(QStringLiteral(
                   "verdict engine=%1 subpixelPositions=%2 binsWithoutPin={%3} historyRowsWithBinsSpread=%4 "
                   "pixelsDifferWithoutPin=%5 pixelsDifferWithPin=%6 binsNot4WithPin=%7 "
                   "recreatedAfterClear=%8 layoutIdentical=%9")
                   .arg(t.engine, t.subpixel ? QStringLiteral("yes") : QStringLiteral("no"),
                        bins.join(QLatin1Char(',')))
                   .arg(t.historyRowsWithBinsSpread)
                   .arg(t.diffWithoutPin)
                   .arg(t.diffWithPin)
                   .arg(t.binsNot4WithPin)
                   .arg(t.recreated ? QStringLiteral("yes") : QStringLiteral("no"),
                        t.layoutIdentical ? QStringLiteral("yes") : QStringLiteral("no")));
        QString h1;
        if (t.diffWithoutPin == 0) {
            h1 = t.subpixel ? QStringLiteral("not-reproduced (no history difference on this engine)")
                            : QStringLiteral("not-applicable (whole-pixel glyph positions)");
        } else if (t.diffWithPin == 0 && t.binsNot4WithPin == 0 &&
                   (t.historyRowsWithBinsSpread > 0 || t.binsWithoutPin.size() > 1)) {
            h1 = QStringLiteral("confirmed (bins depend on the thread's first DPR; pinned bins remove every "
                                "difference)");
        } else if (t.diffWithPin == 0) {
            h1 = QStringLiteral(
                "fixed-by-pin (pinning removes every difference; bins did not visibly spread)");
        } else if (t.historyRowsWithBinsSpread == 0 && t.binsWithoutPin.size() <= 1) {
            h1 = QStringLiteral("refuted (pixels differ with equal bins: see layout, font and log lines)");
        } else {
            h1 = QStringLiteral("partial (pinning leaves differences: see layout, font and log lines)");
        }
        report(QStringLiteral("verdict H1=%1").arg(h1));
        report(QStringLiteral("verdict H2=%1")
                   .arg(t.recreated ? QStringLiteral("engines recreated after a cache clear")
                                    : QStringLiteral("no recreation seen")));
        report(QStringLiteral("verdict H5=%1")
                   .arg(t.fontDatabaseChanges + t.invalidations > 0
                            ? QStringLiteral("font database changed during the run")
                            : QStringLiteral("no font database change")));
        report(QStringLiteral("verdict H7/H8=%1")
                   .arg(t.layoutIdentical ? QStringLiteral("same layout and faces on every thread")
                                          : QStringLiteral("layout or face differs between threads")));
    }

    // Which engine draws each role, with and without hinting, on a fresh thread (nothing drawn there yet).
    void fontEngines() {
        const Fonts fonts = qttest::testFonts();
        for (const bool noHinting : {false, true}) {
            const Conditions conditions(noHinting, qt::detail::glyphBinPinning());
            QStringList lines;
            onFreshThread([&] {
                for (const Role& r : kRoles) {
                    const QFont logical = fonts->font(r.style);
                    const Engines e = enginesOf(logical);
                    if (e.primary == nullptr) {
                        lines.append(
                            QStringLiteral("font role=%1 engine=none").arg(QString::fromLatin1(r.name)));
                        continue;
                    }
                    const QFontInfo info(logical);
                    lines.append(
                        QStringLiteral(
                            "font role=%1 hint=%2 engine=%3 multi=%4 px=%5 weight=%6 hinting=%7 format=%8 "
                            "subpixelX=%9 internalCache=%10 bins=%11 face=%12 synthesized=%13 "
                            "info=\"%14/%15\"")
                            .arg(QString::fromLatin1(r.name),
                                 noHinting ? QStringLiteral("none") : QStringLiteral("default"),
                                 typeName(e.primary->type()),
                                 e.script->type() == QFontEngine::Multi ? QStringLiteral("yes")
                                                                        : QStringLiteral("no"))
                            .arg(e.primary->fontDef.pixelSize)
                            .arg(static_cast<int>(e.primary->fontDef.weight))
                            .arg(hintName(static_cast<int>(e.primary->fontDef.hintingPreference)),
                                 formatName(e.primary->glyphFormat))
                            .arg(e.primary->supportsHorizontalSubPixelPositions() ? 1 : 0)
                            .arg(e.primary->hasInternalCaching() ? 1 : 0)
                            .arg(e.primary->m_subPixelPositionCount)
                            .arg(faceOf(e.primary))
                            .arg(e.primary->synthesized())
                            .arg(info.family(), info.styleName()));
                    if (r.style == plot::TextStyleId::Caption && !noHinting) {
                        tally().engine = typeName(e.primary->type());
                    }
                    if (e.primary->supportsHorizontalSubPixelPositions()) {
                        tally().subpixel = true;
                    }
                }
            });
            for (const QString& l : std::as_const(lines)) {
                report(l);
            }
        }
    }

    // Qt's own count at a first glyph-cache fill, per glyph and scale; and where each glyph's image changes
    // over a pixel (the rasteriser's grid at that scale).
    void emulatedBinCounts() {
        const Fonts fonts = qttest::testFonts();
        for (const bool noHinting : {false, true}) {
            const Conditions conditions(noHinting, qt::detail::glyphBinPinning());
            QStringList lines;
            onFreshThread([&] {
                for (const Role& r : kRoles) {
                    const Engines e = enginesOf(fonts->font(r.style));
                    if (e.primary == nullptr) {
                        continue;
                    }
                    const QFontEngine::GlyphFormat format = e.primary->glyphFormat != QFontEngine::Format_None
                                                                ? e.primary->glyphFormat
                                                                : QFontEngine::Format_A8;
                    for (const char32_t ch : {U'd', U'j', U'f', U'C', U'1'}) {
                        const glyph_t glyph = e.primary->glyphIndex(static_cast<uint>(ch));
                        QStringList counts;
                        QStringList sweeps;
                        for (const qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
                            BinCounter counter(format, QTransform::fromScale(scale, scale));
                            counts.append(
                                QStringLiteral("%1x=%2").arg(scale).arg(counter.binCount(e.primary, glyph)));
                            if (scale == 1.0 || scale == 1.5 || scale == 2.0) {
                                QStringList changes;
                                QImage previous;
                                for (int k = 0; k < 48; ++k) {
                                    QImage img = counter.mapAt(e.primary, glyph, k / 48.0);
                                    if (k == 0 || img != previous) {
                                        changes.append(QString::number(k));
                                    }
                                    previous = std::move(img);
                                }
                                // Every step: the rasteriser takes the offset as it is (FreeType).
                                sweeps.append(QStringLiteral("%1x=%2").arg(scale).arg(
                                    changes.size() == 48
                                        ? QStringLiteral("every")
                                        : QStringLiteral("[%1]").arg(changes.join(QLatin1Char(',')))));
                            }
                        }
                        lines.append(QStringLiteral("emulate role=%1 hint=%2 engine=%3 glyph='%4' count{%5} "
                                                    "imageChangesAt/48{%6}")
                                         .arg(QString::fromLatin1(r.name),
                                              noHinting ? QStringLiteral("none") : QStringLiteral("default"),
                                              typeName(e.primary->type()),
                                              QString(QChar(static_cast<char16_t>(ch))),
                                              counts.join(QLatin1Char(' ')), sweeps.join(QLatin1Char(' '))));
                    }
                }
            });
            for (const QString& l : std::as_const(lines)) {
                report(l);
            }
        }
    }

    // H1: the same DPR 1 (and DPR 2) scene drawn by fresh threads after different DPR histories.
    void threadHistory_data() { addRows(); }
    void threadHistory() {
        QFETCH(bool, noHinting);
        QFETCH(bool, pin);
        const Conditions conditions(noHinting, pin);
        const Fonts fonts = qttest::testFonts();
        const QString row = rowName(noHinting, pin);
        struct History {
            QString name;
            std::vector<float> dprs;
            QImage last;
            QStringList bins; // after each step
            std::set<int> finalBins;
            int not4 = 0;
        };
        std::vector<History> runs = {{QStringLiteral("A"), {2.0F, 1.0F}, {}, {}, {}, 0},
                                     {QStringLiteral("B"), {1.0F}, {}, {}, {}, 0},
                                     {QStringLiteral("C"), {1.5F, 1.0F}, {}, {}, {}, 0},
                                     {QStringLiteral("D"), {1.0F, 2.0F}, {}, {}, {}, 0},
                                     {QStringLiteral("E"), {2.0F}, {}, {}, {}, 0}};
        for (History& h : runs) {
            onFreshThread([&] {
                for (const float dpr : h.dprs) {
                    h.last = onePass(probeScene(dpr), *fonts);
                    h.bins.append(QStringLiteral("after %1x: %2")
                                      .arg(static_cast<double>(dpr))
                                      .arg(binsOfRoles(*fonts)));
                }
                static_cast<void>(binsOfRoles(*fonts, &h.not4, &h.finalBins));
            });
            report(QStringLiteral("history %1 thread=%2 dprs=%3 %4")
                       .arg(row, h.name, dprList(h.dprs), h.bins.join(QStringLiteral("; "))));
            if (pin) {
                tally().binsNot4WithPin += h.not4;
            } else {
                tally().binsWithoutPin.insert(h.finalBins.begin(), h.finalBins.end());
            }
        }
        const QString tag = fileTag(noHinting, pin);
        const std::int64_t ab =
            differing(runs[0].last, runs[1].last, QStringLiteral("history_%1_A_vs_B").arg(tag));
        const std::int64_t cb =
            differing(runs[2].last, runs[1].last, QStringLiteral("history_%1_C_vs_B").arg(tag));
        const std::int64_t de =
            differing(runs[3].last, runs[4].last, QStringLiteral("history_%1_D_vs_E").arg(tag));
        report(QStringLiteral("history %1 differingPixels dpr1: A(2,1)-vs-B(1)=%2 C(1.5,1)-vs-B(1)=%3 "
                              "dpr2: D(1,2)-vs-E(2)=%4")
                   .arg(row)
                   .arg(ab)
                   .arg(cb)
                   .arg(de));
        if (!pin && (runs[0].finalBins != runs[1].finalBins || runs[2].finalBins != runs[1].finalBins ||
                     runs[3].finalBins != runs[4].finalBins)) {
            ++tally().historyRowsWithBinsSpread;
        }
        // What pinning changes against Qt's own count (the pinned row against the unpinned one, threads B and
        // E): how far the text a fresh thread draws moves with the fix.
        static std::vector<QImage> unpinned[2];
        std::vector<QImage>& base = unpinned[noHinting ? 1 : 0];
        if (!pin) {
            base = {runs[1].last, runs[4].last};
        } else if (base.size() == 2) {
            report(QStringLiteral("history %1 pinChangesPixels dpr1: B=%2 dpr2: E=%3")
                       .arg(row)
                       .arg(differing(runs[1].last, base[0], QStringLiteral("pin_effect_%1_B").arg(tag)))
                       .arg(differing(runs[4].last, base[1], QStringLiteral("pin_effect_%1_E").arg(tag))));
        }
        for (const std::int64_t d : {ab, cb, de}) {
            tallyDiff(pin, d);
        }
        if (pin) {
            QCOMPARE(ab, std::int64_t{0});
            QCOMPARE(cb, std::int64_t{0});
            QCOMPARE(de, std::int64_t{0});
            QCOMPARE(runs[0].not4 + runs[1].not4 + runs[2].not4 + runs[3].not4 + runs[4].not4, 0);
        }
    }

    // qtrender.tst_static_strips' failing shape: strips on fresh workers against one pass on a thread whose
    // engines first drew at DPR 1.
    void stripsAgainstOnePass_data() { addRows(); }
    void stripsAgainstOnePass() {
        QFETCH(bool, noHinting);
        QFETCH(bool, pin);
        const Conditions conditions(noHinting, pin);
        const Fonts fonts = qttest::testFonts();
        const QString row = rowName(noHinting, pin);
        for (const float dpr : {2.0F, 1.5F}) {
            const plot::DrawList list = probeScene(dpr);
            QImage single;
            QString singleBins;
            onFreshThread([&] {
                static_cast<void>(onePass(probeScene(1.0F), *fonts));
                single = onePass(list, *fonts);
                singleBins = binsOfRoles(*fonts);
            });
            QImage strips;
            {
                TaskPool pool(4);
                auto f = pool.submit(Priority::Visible, TaskKey{}, CancelToken{}, [&](const CancelToken&) {
                    return qt::RasterBackend::rasteriseStatic(list, *fonts, 0xFFFFFFFFU);
                });
                Result<QImage> r = std::move(f).take();
                QVERIFY(r.ok());
                strips = std::move(r).value();
            }
            const std::int64_t diff = differing(
                strips, single,
                QStringLiteral("strips_%1_dpr%2").arg(fileTag(noHinting, pin)).arg(static_cast<double>(dpr)));
            report(QStringLiteral("strips %1 dpr=%2 differingPixels=%3 onePassThread{%4}")
                       .arg(row)
                       .arg(static_cast<double>(dpr))
                       .arg(diff)
                       .arg(singleBins));
            tallyDiff(pin, diff);
            if (pin) {
                QCOMPARE(diff, std::int64_t{0});
            }
        }
    }

    // H2: a thread's font cache cleared between draws (QFontDatabase::addApplicationFont and other font
    // database changes do this on the GUI thread): the engines come back new, with no bins, and get them at
    // the next DPR.
    void engineRecreation_data() { addRows(); }
    void engineRecreation() {
        QFETCH(bool, noHinting);
        QFETCH(bool, pin);
        const Conditions conditions(noHinting, pin);
        const project::Scheme scheme = qttest::testScheme();
        const QString row = rowName(noHinting, pin);
        QImage first;
        QImage again;
        QString before;
        QString after2;
        QString after1;
        bool recreated = false;
        int not4 = 0;
        onFreshThread([&] {
            const qt::SchemeFonts a(scheme);
            first = onePass(probeScene(1.0F), a);
            before = binsOfRoles(a);
            const QFontEngine* old = enginesOf(a.font(plot::TextStyleId::Caption)).primary;
            QFontCache::instance()->clear();
            const qt::SchemeFonts b(
                scheme); // a new font set: new static texts, laid out with the new engines
            static_cast<void>(onePass(probeScene(2.0F), b));
            after2 = binsOfRoles(b);
            recreated = enginesOf(b.font(plot::TextStyleId::Caption)).primary != old;
            const qt::SchemeFonts c(scheme);
            again = onePass(probeScene(1.0F), c);
            after1 = binsOfRoles(c, &not4);
        });
        const std::int64_t diff =
            differing(again, first, QStringLiteral("recreate_%1").arg(fileTag(noHinting, pin)));
        report(
            QStringLiteral("recreate %1 recreated=%2 bins{at 1x: %3} {after clear, at 2x: %4} {then 1x: %5} "
                           "dpr1 differingPixels=%6")
                .arg(row, recreated ? QStringLiteral("yes") : QStringLiteral("no"), before, after2, after1)
                .arg(diff));
        tally().recreated = tally().recreated || recreated;
        tallyDiff(pin, diff);
        if (pin) {
            tally().binsNot4WithPin += not4;
            QCOMPARE(diff, std::int64_t{0});
            QCOMPARE(not4, 0);
        }
    }

    // H7 and H8: one run's glyphs and positions, and each role's face, on the GUI thread and on a worker.
    void layoutAndFaces() {
        const Fonts fonts = qttest::testFonts();
        const auto describe = [&] {
            QStringList out;
            for (const Role& r : kRoles) {
                QImage device(8, 8, QImage::Format_ARGB32_Premultiplied);
                QPainter p(&device);
                p.setFont(fonts->font(r.style));
                QTextLayout layout(QStringLiteral("Corr Speed 123.4 km/h jounce"), p.font());
                layout.beginLayout();
                layout.createLine();
                layout.endLayout();
                QStringList glyphs;
                for (const QGlyphRun& run : layout.glyphRuns()) {
                    const QList<quint32> indexes = run.glyphIndexes();
                    const QList<QPointF> positions = run.positions();
                    for (qsizetype i = 0; i < indexes.size() && i < positions.size(); ++i) {
                        glyphs.append(
                            QStringLiteral("%1@%2").arg(indexes.at(i)).arg(positions.at(i).x(), 0, 'f', 4));
                    }
                }
                const Engines e = enginesOf(fonts->font(r.style));
                out.append(QStringLiteral("%1 face=%2 glyphs=%3")
                               .arg(QString::fromLatin1(r.name),
                                    e.primary != nullptr ? faceOf(e.primary) : QStringLiteral("-"),
                                    glyphs.join(QLatin1Char(' '))));
            }
            return out;
        };
        const QStringList gui = describe();
        QStringList worker;
        onFreshThread([&] { worker = describe(); });
        for (qsizetype i = 0; i < gui.size(); ++i) {
            const bool same = i < worker.size() && gui.at(i) == worker.at(i);
            report(QStringLiteral("layout %1 sameOnWorker=%2")
                       .arg(gui.at(i).section(QLatin1Char(' '), 0, 1),
                            same ? QStringLiteral("yes") : QStringLiteral("no")));
            if (!same) {
                tally().layoutIdentical = false;
                report(QStringLiteral("layout gui    %1").arg(gui.at(i)));
                report(QStringLiteral("layout worker %1").arg(i < worker.size() ? worker.at(i) : QString()));
            }
        }
    }
};

QTEST_MAIN(ProbeTextEngines)
#include "probe_text_engines.moc"
