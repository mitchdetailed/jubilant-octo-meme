// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-05 (07 section 4.2, 04 section 6.7 item 5; D-127), offscreen: the esses-render executable on
// data/workbooks/circuit-default.workbook.json and fixtures/motec.ld/synthetic-acc-dialect/input.ld,
// worksheet 0, equals the on-screen LayerCache composite of the same worksheet in the real shell for the same
// ViewState (the fastest lap as Main, CTRL+F; zoomed to it, W; the cursor at its start; time domain; the
// light scheme; the golden fonts; DPR 1) within 0.5 % differing pixels (8/255 per channel,
// qt::compareImages). Compared: the time_distance graph alone (--component 0 at the on-screen graph's size)
// and the same graph cut out of the whole-worksheet render (--size = the on-screen canvas), which also checks
// the placement rounding. The worksheet's values_list component has no Qt-free builder yet (esses-render
// draws its placeholder, the shell a live ValuesList widget; 07 section 4.6 lets the component slip), so it
// is not part of the comparison; the graph is not the worksheet's focused component (no focus border, as
// offscreen). Images (and diffs on failure) are written to <build>/tests/ui/artefacts/.

#include "ui_test_support.hpp"

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/image_diff.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kTimeoutMs = 120000;

QString artefactsDir() {
    const QString dir = QString::fromStdWString(
        (std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts").lexically_normal().wstring());
    QDir().mkpath(dir);
    return dir;
}

QString native(const std::filesystem::path& p) {
    return QDir::toNativeSeparators(QString::fromStdWString(p.wstring()));
}

QString essesRender() {
    return QStandardPaths::findExecutable(QStringLiteral("esses-render"),
                                          {QCoreApplication::applicationDirPath()});
}

// Runs esses-render; the output image or a null image (the failure is logged).
QImage runRender(const QStringList& extra, const QString& out) {
    QProcess p;
    QStringList args{native(uitest::repoRoot() / "data" / "workbooks" / "circuit-default.workbook.json"),
                     native(uitest::accFixtureInput()),
                     QStringLiteral("--worksheet"),
                     QStringLiteral("0"),
                     QStringLiteral("--dpr"),
                     QStringLiteral("1"),
                     QStringLiteral("--selection"),
                     QStringLiteral("fastest"),
                     QStringLiteral("--scheme"),
                     QStringLiteral("light"),
                     QStringLiteral("--font-dir"),
                     native(uitest::repoRoot() / "tests" / "golden" / "fonts"),
                     QStringLiteral("-o"),
                     out};
    args << extra;
    p.start(essesRender(), args);
    if (!p.waitForFinished(kTimeoutMs) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        qWarning("esses-render failed (%d): %s %s", p.exitCode(), p.readAllStandardOutput().constData(),
                 p.readAllStandardError().constData());
        return {};
    }
    qInfo("%s", p.readAllStandardOutput().trimmed().constData());
    return QImage(out);
}

bool hasPendingValue(const plot::DrawList& list) {
    for (const plot::TextRun& run : list.texts) {
        const std::string_view t = list.textOf(run);
        if (t.find("\xE2\x80\xA6") != std::string_view::npos || t.find("...") != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

void report(const char* what, const QImage& actual, const QImage& expected, const QString& stem) {
    const qt::ImageDiff d = qt::compareImages(actual, expected, qt::kGoldenChannelTolerance, true);
    actual.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".render.png"));
    expected.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".screen.png"));
    qInfo("%s: %s", what, qPrintable(d.summary()));
    if (!d.passes()) {
        d.diff.save(artefactsDir() + QLatin1Char('/') + stem + QStringLiteral(".diff.png"));
    }
    QVERIFY2(d.passes(), qPrintable(QString::fromLatin1(what) + QStringLiteral(": ") + d.summary()));
}

} // namespace

class TstRenderParity : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    // AC-M1-12: the GUI thread never blocked on a Future (no assertion recorded, any thread).
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() {
        registerBuiltinComponents();
        QVERIFY(uitest::registerGoldenFonts() > 0);
        if (uitest::accFixtureInput().empty()) {
            QSKIP("synthetic-acc-dialect/input.ld is not generated (ctest fixtures.generate_inputs or "
                  "esses-gen-fixtures --inputs-only)");
        }
        QVERIFY2(!essesRender().isEmpty(), "esses-render next to the test executable");
    }

    void essesRenderEqualsTheScreen() {
        QTemporaryDir config;
        QTemporaryDir out;
        auto rt = uitest::makeRuntime(4);
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.resize(1600, 1000);
            QVERIFY(uitest::showAndActivate(w));
            QCOMPARE(w.activeWorksheetIndex(), 0);
            w.openLog(uitest::accFixtureInput());
            QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kTimeoutMs);
            const auto src = w.selection().sources().front();
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src->id), qt::DistanceState::Ready, kTimeoutMs);

            // The esses-render view: the fastest lap (CTRL+F), zoomed to it (W), the cursor at its start.
            TimeDistanceGraph* graph = uitest::graphOf(w);
            QVERIFY(graph != nullptr);
            w.canvas().focusComponent(graph);
            uitest::press(w, Qt::Key_F, Qt::ControlModifier);
            uitest::press(w, Qt::Key_W);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            ws.setCursor(0, qt::CursorSource::Programmatic);
            QCOMPARE(ws.domain(), plot::Domain::Time);
            QCOMPARE(ws.state().zoom.a, 0.0);
            const model::Range* main = w.selection().resolve(w.selection().snapshot().main);
            QVERIFY(main != nullptr);
            QCOMPARE(ws.state().zoom.b, static_cast<double>(main->duration()));
            // Another component holds the worksheet focus: no focus border on the graph (as offscreen).
            ComponentWidget* other = nullptr;
            for (ComponentWidget* c : w.canvas().components()) {
                other = c != graph ? c : other;
            }
            QVERIFY(other != nullptr);
            w.canvas().focusComponent(other);
            QVERIFY(!graph->isFocusedComponent());
            QVERIFY(uitest::waitQuiet(*graph));
            QTRY_VERIFY_WITH_TIMEOUT(!hasPendingValue(graph->layers().dynamicList()), 10000);
            QVERIFY(!graph->pendingStripeShown());
            const QImage screen = graph->layers().composite();
            QVERIFY(!screen.isNull());
            QCOMPARE(screen.devicePixelRatio(), 1.0);
            const QSize canvas = w.canvas().size();
            const QRect placed = w.canvas().placement(graph->spec().rect, QRect(QPoint(0, 0), canvas));
            QCOMPARE(placed.size(), graph->size());
            const QString size = QStringLiteral("%1x%2").arg(canvas.width()).arg(canvas.height());
            qInfo("canvas %s, graph %dx%d at (%d, %d)", qPrintable(size), placed.width(), placed.height(),
                  placed.x(), placed.y());

            // The graph alone: --component 0 at the worksheet size.
            const QImage component = runRender(
                {QStringLiteral("--component"), QStringLiteral("0"), QStringLiteral("--size"), size},
                out.filePath(QStringLiteral("component.png")));
            QVERIFY(!component.isNull());
            report("component 0 (time_distance)", component, screen, QStringLiteral("parity_component0"));

            // The whole worksheet: the graph's rectangle of it.
            const QImage sheet =
                runRender({QStringLiteral("--size"), size}, out.filePath(QStringLiteral("worksheet.png")));
            QVERIFY(!sheet.isNull());
            QCOMPARE(sheet.size(), canvas);
            report("worksheet 0, graph rectangle", sheet.copy(placed), screen,
                   QStringLiteral("parity_worksheet0"));
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstRenderParity)
#include "tst_render_parity.moc"
