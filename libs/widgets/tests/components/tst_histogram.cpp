// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Histogram (04 section 11; D-196, D-198), offscreen: the Static build through LayeredComponent and
// HistogramGeometry (bars of the visible span, bin hit regions; a zoom rebuilds them, a cursor move only the
// Dynamic layer's highlighted bin); overlays outlined beneath Main in their colours; G, S, K and L as spec
// edits; a click on a bin of a tile makes that tile's trace active; the wheel on the worksheet zoom about
// the cursor; Export Data as CSV whose percentages equal a count made here from the source's formula.

#include "components/plots_test_support.hpp"

#include <esses/store/column.hpp>
#include <esses/widgets/components/histogram.hpp>

#include <QTest>
#include <algorithm>
#include <array>
#include <cmath>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

constexpr const char* kBody =
    R"({"traces":[{"channel":"Throttle"}],"bins":{"mode":"even","bandMode":"width","dataMin":{"v":0,"unit":"%"},)"
    R"("dataMax":{"v":100,"unit":"%"},"bandWidth":{"v":10,"unit":"%"},"lessMinBand":false,"greaterMaxBand":false},)"
    R"("yMode":"percent"})";

void buildSummaries(const model::DataSource& src) {
    for (const model::DataLayer& layer : src.layers) {
        for (const model::Channel& ch : layer.channels) {
            QVERIFY(store::seriesValues(ch.series).buildSummary(CancelToken{}).ok());
        }
    }
}

// The device-px rectangle of the highlighted bin (the Dynamic layer's only FilledRect).
std::optional<plot::RectPx> highlighted(const plot::DrawList& dynamic) {
    for (const plot::DrawCmd& c : dynamic.cmds) {
        if (c.kind == plot::CmdKind::FilledRect) {
            return c.rect;
        }
    }
    return std::nullopt;
}

} // namespace

class TstHistogram : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { registerGoldenFonts(); }

    void buildsBinsAndHighlightsTheCursorBinOnTheDynamicLayer() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<Histogram> rig(w, specOf("histogram", kBody));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
        QCOMPARE(rig.c->commandContext(), qt::CommandContext::Summary);
        QCOMPARE(rig.c->channelNames(), QStringList{QStringLiteral("Throttle")});
        // Ten bins, each a hit region.
        const QSize size = rig.c->size();
        int bins = 0;
        int lastBin = -1;
        for (int x = 0; x < size.width(); ++x) {
            const plot::HitResult h = rig.c->hitAt(QPointF(x + 0.5, 0.5 * size.height()));
            if (h.kind == plot::HitKind::Bin && h.bin != lastBin) {
                ++bins;
                lastBin = h.bin;
            }
        }
        QCOMPARE(bins, 10);

        // The cursor at u = 0 (throttle 100 %, no bin) then at a quarter lap (throttle 50 %: bin [50, 60)).
        w.view->setCursor(int(12.5 * kSec), qt::CursorSource::Programmatic);
        const std::optional<plot::RectPx> at50 = highlighted(rig.c->layers().dynamicList());
        QVERIFY(at50.has_value());
        const int submitted = rig.c->staticBuildsSubmitted();
        w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic); // u = 0.1: 50 + 50 cos(36 deg) = 90.5 %
        QVERIFY(staticUntouched(*rig.c, submitted));                 // Cursor -> Dynamic only
        const std::optional<plot::RectPx> at90 = highlighted(rig.c->layers().dynamicList());
        QVERIFY(at90.has_value());
        QVERIFY(at90->x > at50->x); // a bin further right
        QCOMPARE(rig.c
                     ->hitAt(QPointF((static_cast<double>(at90->x) + 1.0) / rig.c->devicePixelRatioF(),
                                     0.5 * size.height()))
                     .bin,
                 9);

        // The worksheet zoom recomputes the bins.
        w.view->zoomTo(plot::XSpan{0.0, 25.0e6});
        QVERIFY(waitQuiet(*rig.c));
        QVERIFY(rig.c->staticBuildsSubmitted() > submitted);
    }

    void overlaysAreOutlinedBeneathMain() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        w.selection.toggleOverlay(lapRef(*src, 2));
        Rig<Histogram> rig(w, specOf("histogram", kBody));
        plot::DrawList last;
        rig.c->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        const plot::Slot overlay = w.selection.snapshot().visibleSlots().back();
        // The overlay in its range key colour, Main in its channel colour (06 section 5.3 item 3).
        const project::Rgba overlayColour = plot::rangeKeyColour(*w.scheme, overlay);
        const plot::Slot main = w.selection.snapshot().visibleSlots().front();
        const project::Rgba mainColour = plot::traceColour(*w.scheme, w.selection.snapshot(), main, 0);
        QVERIFY(countCmds(last, plot::CmdKind::Polyline, overlayColour) > 0); // outlined bars
        QVERIFY(countCmds(last, plot::CmdKind::FilledRect, mainColour) > 0);  // Main filled
        // The outlines come before Main's bars.
        std::size_t firstOutline = last.cmds.size();
        std::size_t lastMain = 0;
        for (std::size_t i = 0; i < last.cmds.size(); ++i) {
            const plot::DrawCmd& c = last.cmds[i];
            if (c.kind == plot::CmdKind::Polyline && c.colour == overlayColour) {
                firstOutline = std::min(firstOutline, i);
            }
            if (c.kind == plot::CmdKind::FilledRect && c.colour == mainColour) {
                lastMain = i;
            }
        }
        QVERIFY(firstOutline < lastMain);
    }

    void cyclesTogglesAndTheActiveTrace() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<Histogram> rig(w, specOf("histogram",
                                     R"({"traces":[{"channel":"Throttle"},{"channel":"Wheel Speed FL"}],)"
                                     R"("bins":{"mode":"even","bandMode":"count","bandCount":8}})"));
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->activeChannel().value_or(QString()), QStringLiteral("Throttle"));
        // G: overlapped <-> tiled.
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QCOMPARE(rig.c->body().layout, std::string("tiled"));
        QVERIFY(waitQuiet(*rig.c));
        // A click on a bin of the second tile makes its trace active.
        const QSize size = rig.c->size();
        bool clicked = false;
        for (int x = size.width() - 1; x > 0 && !clicked; --x) {
            for (int y = size.height() - 1; y > 0 && !clicked; y -= 4) {
                const plot::HitResult h = rig.c->hitAt(QPointF(x, y));
                if (h.kind == plot::HitKind::Bin && h.trace == 1) {
                    click(rig.c, QPointF(x, y));
                    clicked = true;
                }
            }
        }
        QVERIFY(clicked);
        QCOMPARE(rig.c->activeChannel().value_or(QString()), QStringLiteral("Wheel Speed FL"));
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_layout"), {}));
        QCOMPARE(rig.c->body().layout, std::string("overlapped"));
        // S: bar -> line -> text -> bar.
        for (const char* style : {"line", "text", "bar"}) {
            QVERIFY(rig.c->handleCommand(QStringLiteral("component.cycle_style"), {}));
            QCOMPARE(rig.c->body().style, std::string(style));
        }
        QVERIFY(rig.c->handleCommand(QStringLiteral("component.colour_legend"), {}));
        QVERIFY(rig.c->spec().legend.colour);
        // L: no channel legend is drawn, so the histogram declines it (D-203 (h)).
        QVERIFY(!rig.c->handleCommand(QStringLiteral("component.legend"), {}));
        QVERIFY(rig.c->spec().legend.channels);
        QVERIFY(!rig.c->handleCommand(QStringLiteral("component.colour_band"), {QVariant(1)})); // D-198
        QVERIFY(!rig.c->handleCommand(QStringLiteral("cursor.style"), {}));
        // ENTER / drop: a trace is appended once.
        const int edits = rig.edits;
        rig.c->addChannel(QStringLiteral("Wheel Speed FR"), false);
        rig.c->addChannel(QStringLiteral("Wheel Speed FR"), true);
        QCOMPARE(rig.edits, edits + 1);
        QCOMPARE(rig.c->body().traces.size(), std::size_t{3});
    }

    void gatingAndTheColourChannel() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        constexpr const char* kCount =
            R"({"traces":[{"channel":"Throttle"}],"bins":{"mode":"even","bandMode":"width","dataMin":{"v":0,"unit":"%"},)"
            R"("dataMax":{"v":100,"unit":"%"},"bandWidth":{"v":10,"unit":"%"},"lessMinBand":false,"greaterMaxBand":false},)"
            R"("yMode":"count"})";
        Rig<Histogram> rig(w, specOf("histogram", kCount));
        QVERIFY(waitQuiet(*rig.c));
        const auto counts = [&] {
            MemorySink sink;
            const bool ok = rig.c->exportData(sink, ExportOptions{});
            std::vector<double> out;
            const QList<QByteArray> lines = csvLines(sink.bytes);
            for (qsizetype i = 1; ok && i < lines.size(); ++i) {
                out.push_back(lines[i].split(',')[2].toDouble());
            }
            return out;
        };
        const std::vector<double> before = counts();
        QCOMPARE(before.size(), std::size_t{10});
        // Gating by the throttle drops its one sample at exactly 0 % (half way through the lap) from bin [0,
        // 10).
        plot::HistogramBody body = rig.c->body();
        body.gatingChannel = "Throttle";
        project::ComponentSpec spec = rig.c->spec();
        spec.body = body.toJson();
        rig.c->applySpec(spec);
        QVERIFY(waitQuiet(*rig.c));
        const std::vector<double> after = counts();
        QCOMPARE(after.size(), std::size_t{10});
        QCOMPARE(after[0], before[0] - 1.0);
        for (std::size_t j = 1; j < after.size(); ++j) {
            QCOMPARE(after[j], before[j]);
        }
        // A colour channel splits the bars by its bands; K shows its legend.
        plot::DrawList last;
        rig.c->setBuildObserver([&last](const plot::ComponentBuild& b) { last = b.list; });
        rig.c->onViewChanged(w.view->state(), plot::mask(plot::Change::Data));
        QVERIFY(waitQuiet(*rig.c));
        const int plainBars = countCmds(last, plot::CmdKind::FilledRect);
        body = rig.c->body();
        body.gatingChannel.clear();
        body.colourChannel = "Wheel Speed FL";
        body.colourAxis = analysis::BandAxis{};
        body.colourAxis->bandMode = analysis::BandMode::Count;
        body.colourAxis->bandCount = 3;
        spec = rig.c->spec();
        spec.body = body.toJson();
        spec.legend.colour = true;
        rig.c->applySpec(spec);
        QVERIFY(waitQuiet(*rig.c));
        QCOMPARE(rig.c->channelNames().size(), 2);
        QVERIFY(textsOf(last, plot::TextStyleId::ColourBand).size() >= 3); // the legend: a label per band
        QVERIFY(countCmds(last, plot::CmdKind::FilledRect) > plainBars);   // sub-bars and swatches
    }

    void theWheelZoomsTheWorksheetAboutTheCursor() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        Rig<Histogram> rig(w, specOf("histogram", kBody));
        QVERIFY(waitQuiet(*rig.c));
        const plot::XSpan before = w.view->state().zoom;
        sendWheel(rig.c, QPointF(rig.c->width() / 2.0, rig.c->height() / 2.0), 120);
        const plot::XSpan after = w.view->state().zoom;
        QVERIFY(std::abs(after.length() - 0.8 * before.length()) < 1.0);
        const double c = 10.0e6;
        QVERIFY(std::abs((c - after.a) - 0.8 * (c - before.a)) < 1.0);
    }

    void exportsTheBinsAsCsv() {
        World w;
        const auto src = w.publish(SourceOptions{});
        buildSummaries(*src);
        selectMain(w, lapRef(*src, 1));
        Rig<Histogram> rig(w, specOf("histogram", kBody));
        QVERIFY(waitQuiet(*rig.c));
        MemorySink sink;
        QVERIFY(rig.c->exportData(sink, ExportOptions{}));
        const QList<QByteArray> lines = csvLines(sink.bytes);
        QCOMPARE(lines.size(), 11);
        QCOMPARE(lines.front(), QByteArray("bin_lo,bin_hi,\"Throttle@Main\""));
        // Brute force: the 2500 samples of the lap in bins of 10 %, the last bin closed on 100 % (D-194 as
        // amended by D-203 (d): a sample at dataMax counts).
        const SourceShape shape;
        std::array<int, 10> counts{};
        int inBins = 0;
        for (std::size_t i = 40 * 50; i < 90 * 50; ++i) {
            const double v = shape.throttle(i);
            if (v >= 0.0 && v <= 100.0) {
                ++counts[std::min<std::size_t>(9, static_cast<std::size_t>(std::floor(v / 10.0)))];
                ++inBins;
            }
        }
        double sum = 0.0;
        for (int j = 0; j < 10; ++j) {
            const QList<QByteArray> f = lines[j + 1].split(',');
            QCOMPARE(f[0].toDouble(), 10.0 * j);
            QCOMPARE(f[1].toDouble(), 10.0 * (j + 1));
            const double expected = 100.0 * counts[static_cast<std::size_t>(j)] / inBins;
            QVERIFY2(std::abs(f[2].toDouble() - expected) < 1e-6, f[2].constData());
            sum += f[2].toDouble();
        }
        QVERIFY(std::abs(sum - 100.0) < 1e-6);
    }
};

QTEST_MAIN(TstHistogram)
#include "tst_histogram.moc"
