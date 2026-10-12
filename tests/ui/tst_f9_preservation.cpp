// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-09 (07 section 4.2, 04 section 3.4, 06 section 4.5; D-016), offscreen, in the real shell with two
// logs open through the SourceLoader: the public ACC stand-in (Corr Dist from SPEED) and a written circuit
// log without any speed channel (no Corr Dist). F9 goes through the KeyEventRouter from the worksheet.
//   With Corr Dist: the visible span converts through the Main lap's DistanceIndex (a' = distAt(R.start + a))
//   and F9 back returns it within one sample (1 ms at 1 kHz); no banner.
//   Without Corr Dist: the fraction rule a' = R.start' + (a - R.start) / len(R) * len(R') (len(R') = 1 and
//   R.start' = 0 without an index or track map, both relative to the Main range) and back losslessly; the
//   worksheet reports noDistance and the graph's Chrome layer shows "No distance data for <file>" for that
//   source, also when it is only an overlay under a Main that has distance data.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr Micros kSec = 1'000'000;
constexpr Micros kAccSample = 1000; // 1 kHz SPEED -> Corr Speed / Corr Dist
constexpr int kTimeoutMs = 120000;

bool chromeSays(const TimeDistanceGraph& g, std::string_view needle) {
    const plot::DrawList& list = g.layers().chromeList();
    for (const plot::TextRun& run : list.texts) {
        if (list.textOf(run).find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

struct Shell {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<qt::AppRuntime> rt = uitest::makeRuntime(4);
    std::unique_ptr<MainWindow> w;
    std::shared_ptr<const model::DataSource> acc;    // Corr Dist present
    std::shared_ptr<const model::DataSource> nodist; // no speed channel

    bool open(const std::filesystem::path& accLd) {
        w = std::make_unique<MainWindow>(*rt, uitest::pathsFor(config.path()));
        w->resize(1600, 1000);
        if (!uitest::showAndActivate(*w)) {
            return false;
        }
        qt::test::SyntheticOptions o;
        o.path = "no-speed.ld";
        o.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
        o.wheelSpeeds = false;
        std::filesystem::path noSpeed;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
            const auto mem = qt::test::makeSource(o, rt->cache());
            noSpeed = qt::test::writeLd(*mem, std::filesystem::path(logs.path().toStdWString()), "no-speed");
        }
        w->openLog(accLd);
        w->openLog(noSpeed);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == 2; }, kTimeoutMs)) {
            return false;
        }
        for (const auto& s : w->selection().sources()) {
            (s->prov.path.filename() == noSpeed.filename() ? nodist : acc) = s;
        }
        return acc != nullptr && nodist != nullptr &&
               QTest::qWaitFor(
                   [&] {
                       return w->distance().state(acc->id) == qt::DistanceState::Ready &&
                              w->distance().state(nodist->id) == qt::DistanceState::NoDistance;
                   },
                   kTimeoutMs);
    }
    ~Shell() {
        w.reset();
        rt->shutdown();
    }
};

// The flying lap `index` (0 = the first flying lap) of `src` in Data-window order.
model::RangeRef flyingLap(MainWindow& w, const model::DataSource& src, int index) {
    int n = 0;
    for (const model::RangeRef& r : w.selection().displayedRanges(src.id)) {
        const model::Range* x = w.selection().resolve(r);
        if (x != nullptr && x->lapClass == model::LapClass::Flying && n++ == index) {
            return r;
        }
    }
    return {};
}

} // namespace

class TstF9Preservation : public QObject {
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
        if (uitest::accFixtureInput().empty()) {
            QSKIP("synthetic-acc-dialect/input.ld is not generated (ctest fixtures.generate_inputs or "
                  "esses-gen-fixtures --inputs-only)");
        }
    }

    // With Corr Dist: through the Main lap's DistanceIndex and back within one sample.
    void throughTheDistanceIndex() {
        Shell s;
        QVERIFY(s.open(uitest::accFixtureInput()));
        MainWindow& w = *s.w;
        const model::RangeRef main = flyingLap(w, *s.acc, 3);
        QVERIFY(main.src == s.acc->id);
        w.selection().setMain(main);
        const model::Range* r = w.selection().resolve(main);
        QVERIFY(r != nullptr);
        qt::WorksheetViewModel& ws = *w.activeWorksheet();
        TimeDistanceGraph* graph = uitest::graphOf(w);
        QVERIFY(graph != nullptr);
        w.canvas().focusComponent(graph);
        QCOMPARE(ws.domain(), plot::Domain::Time);
        const plot::XSpan visible{37.5 * kSec, 151.25 * kSec}; // lap-relative, not on sample boundaries
        ws.zoomTo(visible);
        QCOMPARE(ws.state().zoom, visible);
        // Every lap index of the outing arrives from the Background prefetch (no request needed).
        QTRY_VERIFY_WITH_TIMEOUT(w.distance().chain(main, Priority::Interactive) != nullptr &&
                                     w.distance().chain(main, Priority::Interactive)->complete(),
                                 kTimeoutMs);
        const auto chain = w.distance().chain(main, Priority::Interactive);

        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(!ws.noDistance());
        const plot::XSpan d = ws.state().zoom;
        QVERIFY(std::abs(d.a - chain->distAt(r->start + static_cast<Micros>(visible.a))) < 1e-6);
        QVERIFY(std::abs(d.b - chain->distAt(r->start + static_cast<Micros>(visible.b))) < 1e-6);
        QVERIFY(d.a > 0.0 && d.b > d.a && d.b < chain->distAt(r->end));
        QVERIFY(uitest::waitQuiet(*graph));
        QVERIFY(!chromeSays(*graph, "No distance data"));

        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws.domain(), plot::Domain::Time);
        const plot::XSpan back = ws.state().zoom;
        QVERIFY2(std::abs(back.a - visible.a) <= kAccSample, "the span returns within one sample");
        QVERIFY2(std::abs(back.b - visible.b) <= kAccSample, "the span returns within one sample");
        qInfo("F9 with Corr Dist: [%.6f, %.6f] s -> [%.3f, %.3f] m -> [%.6f, %.6f] s", visible.a / 1e6,
              visible.b / 1e6, d.a, d.b, back.a / 1e6, back.b / 1e6);
    }

    // Without Corr Dist: the fraction rule both ways and the banner for that source.
    void fractionRuleAndBannerWithoutDistance() {
        Shell s;
        QVERIFY(s.open(uitest::accFixtureInput()));
        MainWindow& w = *s.w;
        const model::RangeRef main = flyingLap(w, *s.nodist, 1); // the 48 s lap
        QVERIFY(main.src == s.nodist->id);
        w.selection().setMain(main);
        const model::Range* r = w.selection().resolve(main);
        QVERIFY(r != nullptr);
        QCOMPARE(r->duration(), 48 * kSec);
        qt::WorksheetViewModel& ws = *w.activeWorksheet();
        TimeDistanceGraph* graph = uitest::graphOf(w);
        QVERIFY(graph != nullptr);
        w.canvas().focusComponent(graph);
        const plot::XSpan visible{12.0 * kSec, 30.0 * kSec};
        ws.zoomTo(visible);
        QSignalSpy banner(&ws, &qt::WorksheetViewModel::noDistanceData);

        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(ws.noDistance());
        QVERIFY(!banner.isEmpty() && banner.last().at(0).toBool());
        // a' = R.start' + (a - R.start) / len(R) * len(R'), with R.start' = 0 and len(R') = 1.
        const double len = static_cast<double>(r->duration());
        QVERIFY(std::abs(ws.state().zoom.a - (0.0 + visible.a / len * 1.0)) < 1e-12);
        QVERIFY(std::abs(ws.state().zoom.b - (0.0 + visible.b / len * 1.0)) < 1e-12);
        QVERIFY(uitest::waitQuiet(*graph));
        const std::string name = s.nodist->prov.path.filename().string();
        QTRY_VERIFY(chromeSays(*graph, "No distance data for " + name));
        QVERIFY(!chromeSays(*graph, "No distance data for " + s.acc->prov.path.filename().string()));

        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws.domain(), plot::Domain::Time);
        QCOMPARE(ws.state().zoom, visible); // lossless
        QVERIFY(!ws.noDistance());
        QVERIFY(!banner.last().at(0).toBool());

        // The same source as Overlay 1 under a Main with distance data: the banner names it, the Main span
        // still converts through the Main lap's index.
        const model::RangeRef accMain = flyingLap(w, *s.acc, 0);
        w.selection().setMain(accMain);
        w.selection().toggleOverlay(main);
        QCOMPARE(w.selection().overlayNumber(main), 1);
        uitest::press(w, Qt::Key_F9);
        QCOMPARE(ws.domain(), plot::Domain::Distance);
        QVERIFY(!ws.noDistance());
        QVERIFY(uitest::waitQuiet(*graph));
        QTRY_VERIFY(chromeSays(*graph, "No distance data for " + name));
    }
};

QTEST_MAIN(TstF9Preservation)
#include "tst_f9_preservation.moc"
