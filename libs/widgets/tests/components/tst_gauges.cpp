// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// NumericGauge and BarGauge (04 sections 16-17; D-198), offscreen: the value at the cursor for Main and the
// reference overlay (expected values from the synthetic source's formula), a cursor move rebuilding only the
// Dynamic layer, colour bands colouring the text, the background or the bar, ALT+n toggling band n as an
// undoable spec edit (bands beyond the list are no-ops), the bar from its origin to the value with the
// reference marker, the channel drop replacing the channel, Export Data as CSV and renderToImage.

#include "components/plots_test_support.hpp"

#include <esses/plotcore/gauge_geometry.hpp>
#include <esses/store/column.hpp>
#include <esses/widgets/components/bar_gauge.hpp>
#include <esses/widgets/components/numeric_gauge.hpp>

#include <QTest>
#include <array>
#include <charconv>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

std::string fixed(double v, int dps) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, dps);
    return {buf.data(), r.ptr};
}

void buildSummaries(const model::DataSource& src) {
    for (const model::DataLayer& layer : src.layers) {
        for (const model::Channel& ch : layer.channels) {
            QVERIFY(store::seriesValues(ch.series).buildSummary(CancelToken{}).ok());
        }
    }
}

std::vector<const plot::TextRun*> runsOf(const plot::DrawList& list, plot::TextStyleId style) {
    std::vector<const plot::TextRun*> out;
    for (const plot::TextRun& r : list.texts) {
        if (r.style == style) {
            out.push_back(&r);
        }
    }
    return out;
}

std::vector<plot::RectPx> rectsOf(const plot::DrawList& list, project::Rgba colour) {
    std::vector<plot::RectPx> out;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind == plot::CmdKind::FilledRect && c.colour == colour) {
            out.push_back(c.rect);
        }
    }
    return out;
}

// The throttle of lap `lap` at `seconds` into it (the 50 Hz sample there).
double throttleAt(std::size_t lap, double seconds) {
    const SourceShape shape;
    return shape.throttle(static_cast<std::size_t>(std::llround((shape.lapStart(lap) + seconds) * 50.0)));
}

constexpr const char* kNumeric =
    R"({"channel":"Throttle","displayDps":1,"colourBands":[{"min":{"v":80,"unit":"%"},"max":{"v":0,"unit":"%"},)"
    R"("open":"above","colour":"#e53935","visible":true},{"min":{"v":0,"unit":"%"},"max":{"v":20,"unit":"%"},)"
    R"("open":"none","colour":"#1e88e5","visible":true}]})";

constexpr const char* kBar =
    R"({"channel":"Throttle","scale":{"mode":"manual","min":{"v":0,"unit":"%"},"max":{"v":100,"unit":"%"}},)"
    R"("barColour":"#123456","colourBands":[{"min":{"v":80,"unit":"%"},"max":{"v":0,"unit":"%"},"open":"above",)"
    R"("colour":"#e53935","visible":true}],"bandMode":"bar"})";

} // namespace

class TstGauges : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void numericShowsMainAndReferenceAtTheCursor() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));              // a 50 s lap
        w.selection.toggleOverlay0(lapRef(*src, 2)); // a 48 s lap: the reference overlay
        Rig<NumericGauge> rig(w, specOf("gauge_numeric", kNumeric), QSize(240, 160));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::MainAndReference);
        QCOMPARE(rig.c->commandContext(), qt::CommandContext::Component);
        QCOMPARE(rig.c->activeChannel().value_or(QString()), QStringLiteral("Throttle"));
        QCOMPARE(rig.c->channelNames(), QStringList{QStringLiteral("Throttle")});
        // The value texts of the current Dynamic layer (empty strings while a chunk is pending).
        const auto texts = [&] { return textsOf(rig.c->layers().dynamicList(), plot::TextStyleId::Gauge); };
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        const std::vector<std::string> expected10{fixed(throttleAt(1, 10.0), 1),
                                                  fixed(throttleAt(2, 10.0), 1)};
        QTRY_VERIFY_WITH_TIMEOUT(texts() == expected10, kTimeoutMs); // the overlay aligned at lap start
        {
            const plot::DrawList& d = rig.c->layers().dynamicList();
            const std::vector<const plot::TextRun*> values = runsOf(d, plot::TextStyleId::Gauge);
            QCOMPARE(values.size(), std::size_t{2});
            QCOMPARE(values[1]->colour, w.scheme->role("overlay0", 0));
            QVERIFY(values[0]->scale > 0.0F);
            QVERIFY(values[0]->y < values[1]->y); // the reference beneath
        }
        // A cursor move: Dynamic only.
        const int submitted = rig.c->staticBuildsSubmitted();
        w.view->setCursor(25 * kSec, qt::CursorSource::Programmatic);
        QVERIFY(staticUntouched(*rig.c, submitted));
        QTRY_VERIFY_WITH_TIMEOUT(!texts().empty() && texts().front() == fixed(throttleAt(1, 25.0), 1),
                                 kTimeoutMs);
        // A zoom changes nothing a gauge draws: no Static build either.
        w.view->zoomTo(plot::XSpan{0.0, 10.0e6});
        QVERIFY(staticUntouched(*rig.c, submitted));
    }

    void numericBandsAndAltN() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<NumericGauge> rig(w, specOf("gauge_numeric", kNumeric), QSize(240, 160));
        QVERIFY(waitQuiet(*rig.c));
        // At the lap start the throttle is 100 %: band 0 (>= 80 %) colours the text.
        w.view->setCursor(0, qt::CursorSource::Programmatic);
        const auto valueColour = [&] {
            const std::vector<const plot::TextRun*> v =
                runsOf(rig.c->layers().dynamicList(), plot::TextStyleId::Gauge);
            return v.empty() ? project::Rgba{0} : v.front()->colour;
        };
        QTRY_COMPARE_WITH_TIMEOUT(valueColour(), project::Rgba{0xE53935FFU}, kTimeoutMs);
        // ALT+0 hides band 0 (one undoable spec edit): the foreground colour.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.colour_band"), {QVariant(0)}));
        QCOMPARE(rig.edits, 1);
        QVERIFY(!rig.c->body().colourBands[0].visible);
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(valueColour(), w.scheme->role("cptForeground", 0));
        // Half a lap in, the throttle is 0 %: band 1 [0, 20) colours it.
        w.view->setCursor(25 * kSec, qt::CursorSource::Programmatic);
        QTRY_COMPARE_WITH_TIMEOUT(valueColour(), project::Rgba{0x1E88E5FFU}, kTimeoutMs);
        // ALT+9: beyond the list, handled as a no-op.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.colour_band"), {QVariant(9)}));
        QCOMPARE(rig.edits, 1);
        // bandMode background: the band fills the content.
        project::ComponentSpec spec = rig.c->spec();
        plot::GaugeNumericBody body = rig.c->body();
        body.bandMode = "background";
        spec.body = body.toJson();
        rig.c->applySpec(spec);
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rectsOf(rig.c->layers().dynamicList(), 0x1E88E5FFU).size(), std::size_t{1});
        QCOMPARE(valueColour(), w.scheme->role("cptForeground", 0));
        // The channel drop replaces the channel.
        rig.c->addChannel(QStringLiteral("Wheel Speed FL"), false);
        QCOMPARE(rig.c->body().channel, std::string("Wheel Speed FL"));
        QCOMPARE(rig.c->channelNames(), QStringList{QStringLiteral("Wheel Speed FL")});
    }

    void barFromOriginToValueWithReferenceMarker() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        w.selection.toggleOverlay(lapRef(*src, 2)); // Overlay 1: the reference without an Overlay 0
        Rig<BarGauge> rig(w, specOf("gauge_bar", kBar), QSize(140, 300));
        plot::DrawList staticList;
        rig.c->setBuildObserver([&staticList](const plot::ComponentBuild& b) { staticList = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::MainAndReference);
        const std::vector<plot::RectPx> tracks = rectsOf(staticList, w.scheme->role("keyBackground", 0));
        QCOMPARE(tracks.size(), std::size_t{1});
        const plot::RectPx track = tracks.front();
        const auto yOf = [&](double v) {
            return track.y + static_cast<float>((1.0 - v / 100.0) * static_cast<double>(track.h));
        };
        // 12.5 s into the lap the throttle is 50 %: a bar in barColour from 0 % up to it.
        w.view->setCursor(int(12.5 * kSec), qt::CursorSource::Programmatic);
        const double v = throttleAt(1, 12.5);
        QTRY_COMPARE_WITH_TIMEOUT(rectsOf(rig.c->layers().dynamicList(), 0x123456FFU).size(), std::size_t{1},
                                  kTimeoutMs);
        const plot::RectPx bar = rectsOf(rig.c->layers().dynamicList(), 0x123456FFU).front();
        QCOMPARE(bar.x, track.x);
        QVERIFY(std::abs(bar.y - yOf(v)) < 1e-3F);
        QVERIFY(std::abs(bar.y + bar.h - yOf(0.0)) < 1e-3F);
        // The reference overlay's marker in Overlay 1's key colour (06 section 5.3 item 3) at its value.
        const plot::Slot ref = w.selection.snapshot().visibleSlots().back();
        const project::Rgba refColour = plot::rangeKeyColour(*w.scheme, ref);
        bool marker = false;
        const plot::DrawList& d = rig.c->layers().dynamicList();
        for (const plot::DrawCmd& c : d.cmds) {
            if (c.kind == plot::CmdKind::Lines && c.colour == refColour) {
                QVERIFY(std::abs(d.verts[2 * c.vertexOffset + 1] - std::round(yOf(throttleAt(2, 12.5)))) <
                        1e-3F);
                marker = true;
            }
        }
        QVERIFY(marker);
        // At the lap start (100 %) bandMode bar recolours the bar with band 0.
        const int submitted = rig.c->staticBuildsSubmitted();
        w.view->setCursor(0, qt::CursorSource::Programmatic);
        QVERIFY(staticUntouched(*rig.c, submitted)); // Dynamic only
        QTRY_COMPARE_WITH_TIMEOUT(rectsOf(rig.c->layers().dynamicList(), 0xE53935FFU).size(), std::size_t{1},
                                  kTimeoutMs);
        QCOMPARE(rig.c->staticBuildsSubmitted(), submitted);
        // ALT+0 hides the band: barColour again; the band's line leaves the Static layer.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.colour_band"), {QVariant(0)}));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rectsOf(rig.c->layers().dynamicList(), 0x123456FFU).size(), std::size_t{1});
        QCOMPARE(countCmds(staticList, plot::CmdKind::Lines, 0xE53935FFU), 0);
        QVERIFY(!rig.c->renderToImage(rig.c->devicePixelRatioF()).isNull());
        QVERIFY(!rig.c->renderToImage(2.0 * rig.c->devicePixelRatioF()).isNull());
    }

    void gaugesExportTheirChannelAsCsv() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<BarGauge> rig(w, specOf("gauge_bar", kBar), QSize(140, 300));
        QVERIFY(waitQuiet(*rig.c));
        MemorySink sink;
        QVERIFY(rig.c->exportData(sink, ExportOptions{}));
        const QList<QByteArray> lines = csvLines(sink.bytes);
        QCOMPARE(lines.front(), QByteArray("Time [s],\"Throttle [%]\""));
        QCOMPARE(lines.size(), 1 + 50 * 50);
        const QList<QByteArray> f = lines[1].split(',');
        QCOMPARE(f[0].toDouble(), 40.0);
        QVERIFY(std::abs(f[1].toDouble() - throttleAt(1, 0.0)) < 1e-9);
        // Selected range padded by 2 s.
        MemorySink padded;
        ExportOptions o;
        o.range = ExportOptions::Range::SelectedPadded;
        QVERIFY(rig.c->exportData(padded, o));
        QCOMPARE(csvLines(padded.bytes).size(), 1 + 54 * 50);
        Rig<NumericGauge> numeric(w, specOf("gauge_numeric", kNumeric), QSize(240, 160));
        QVERIFY(waitQuiet(*numeric.c));
        MemorySink n;
        QVERIFY(numeric.c->exportData(n, ExportOptions{}));
        QCOMPARE(csvLines(n.bytes).size(), 1 + 50 * 50);
    }
};

QTEST_MAIN(TstGauges)
#include "tst_gauges.moc"
