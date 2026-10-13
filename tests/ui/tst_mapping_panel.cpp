// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-11 in the real shell (05 section 8.1, 06 section 13.8; D-168, D-179), offscreen, as the M2 demo opens
// a CSV beside an .ld file:
//   unknownUnitsBlockUntilResolved  esses with a project: a CSV with two unknown units opens beside the .ld;
//                                   the Mapping panel appears above the worksheet area by itself and the CSV
//                                   is not published (the .ld is) while Resolve refuses incomplete rows; the
//                                   rows are completed, Resolve is one "Resolve mapping" step (CTRL+Z takes
//                                   it back, CTRL+Y redoes it) and the file publishes with the units; CTRL+S
//                                   writes channels/unitmaps/generic.csv.json; another CSV with the same
//                                   units opens silently (the panel never shows).
//   saveToUnitMapThenOpenAnyway     Save completes the project's unit map without opening the file (the panel
//                                   stays); Open Anyway opens it; a later file opens silently.
// Every runtime marks the GUI thread NoBlock as esses does; no ESSES_ASSERT may fail on any thread.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/formats/csv.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/editors_data/mapping_panel.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <fstream>
#include <memory>
#include <string_view>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

// Two unknown units (zorks, blips) and one the units table knows.
constexpr std::string_view kCsv = "Time,Boost,Coolant,Speed\n"
                                  "s,zorks,blips,km/h\n"
                                  "0,100,20,36\n"
                                  "0.5,150,21,72\n"
                                  "1.0,200,22,108\n"
                                  "1.5,210,23,110\n";

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

// Each file gets a last row of its own: identical content would be the same source (its quick hash).
std::filesystem::path writeCsv(const std::filesystem::path& dir, const std::string& name) {
    static int serial = 0;
    const std::filesystem::path p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out << kCsv << "2.0," << 220 + (++serial) << ",24,111\n";
    return p;
}

std::shared_ptr<const model::DataSource> sourceNamed(MainWindow& w, const std::string& file) {
    for (const auto& s : w.selection().sources()) {
        if (s->prov.path.filename().string() == file) {
            return s;
        }
    }
    return nullptr;
}

const model::Channel* channel(const model::DataSource& src, std::string_view name) {
    const model::DataLayer* l = src.layer(model::kNormalLayer);
    return l == nullptr ? nullptr : l->find(name);
}

int rowOf(MappingPanel& panel, const char* loggedName) {
    for (int i = 0; i < static_cast<int>(panel.rows().size()); ++i) {
        if (panel.rows()[static_cast<std::size_t>(i)].loggedName == loggedName) {
            return i;
        }
    }
    return -1;
}

struct Project {
    QTemporaryDir root;
    QTemporaryDir config;
    std::filesystem::path folder;
    std::filesystem::path logs;
    std::filesystem::path ld;

    bool prepare(qt::AppRuntime& rt) {
        if (!root.isValid() || !config.isValid()) {
            return false;
        }
        folder = fsPath(root.path()) / "Demo";
        logs = fsPath(root.path()) / "logs";
        std::filesystem::create_directories(logs);
        const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
        qt::test::SyntheticOptions o;
        o.path = "demo.ld";
        ld = qt::test::writeLd(*qt::test::makeSource(o, rt.cache()), logs, "demo");
        return true;
    }
};

std::unique_ptr<MainWindow> shell(qt::AppRuntime& rt, const Project& p) {
    auto w = std::make_unique<MainWindow>(rt, uitest::pathsFor(p.config.path()));
    w->setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
    if (!w->session().newProject(p.folder, "Demo").ok() || !w->openProject(p.folder) ||
        !uitest::showAndActivate(*w)) {
        return nullptr;
    }
    return w;
}

} // namespace

class TstMappingPanel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() { registerBuiltinComponents(); }

    void unknownUnitsBlockUntilResolved() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        {
            auto w = shell(*rt, p);
            QVERIFY(w != nullptr);
            MappingPanel& panel = w->mappingPanel();
            QVERIFY(!panel.isVisible()); // nothing waits
            // esses -l demo.ld -l other_logger.csv
            w->openLog(p.ld);
            w->openLog(writeCsv(p.logs, "other_logger.csv"));
            QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
            QVERIFY(panel.isVisible());
            QVERIFY(panel.geometry().bottom() <= w->canvas().geometry().top()); // above the worksheet area
            QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(*w, "demo.ld") != nullptr, 30000);
            QTest::qWait(100);
            QVERIFY(sourceNamed(*w, "other_logger.csv") == nullptr); // blocked until resolved
            QCOMPARE(panel.rows().size(), std::size_t{2});
            QVERIFY(!panel.resolve()); // incomplete rows
            QVERIFY(!panel.lastError().isEmpty());
            QCOMPARE(w->session().undoStack().count(), 0);

            const int boost = rowOf(panel, "Boost");
            const int coolant = rowOf(panel, "Coolant");
            QVERIFY(boost >= 0 && coolant >= 0);
            QVERIFY(panel.setQuantity(boost, QStringLiteral("Pressure")));
            QVERIFY(!panel.setUnit(boost, QStringLiteral("deg C"))); // another quantity
            QVERIFY(panel.setUnit(boost, QStringLiteral("kPa")));
            QVERIFY(panel.setUnit(coolant, QStringLiteral("deg C"))); // the unit sets the quantity
            QVERIFY(panel.resolve());
            QVERIFY(!panel.blocksPublish());
            QTRY_VERIFY(!panel.isVisible());
            QCOMPARE(w->session().undoStack().undoText(), QStringLiteral("Resolve mapping"));
            QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(*w, "other_logger.csv") != nullptr, 30000);
            auto csv = sourceNamed(*w, "other_logger.csv");
            QVERIFY(channel(*csv, "Boost") != nullptr);
            QCOMPARE(qt::test::shippedUnits()->quantity(channel(*csv, "Boost")->desc.quantity).name,
                     std::string("Pressure"));
            QCOMPARE(qt::test::shippedUnits()->quantity(channel(*csv, "Coolant")->desc.quantity).name,
                     std::string("Temperature"));
            QCOMPARE(w->selection().sources().size(), std::size_t{2}); // beside the .ld

            // CTRL+Z takes the decision back (the project's unit map is empty again), CTRL+Y redoes it.
            w->returnFocus();
            uitest::press(*w, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(w->session().unitMaps().count("generic.csv"), std::size_t{0});
            uitest::press(*w, Qt::Key_Y, Qt::ControlModifier);
            QCOMPARE(w->session().unitMaps().count("generic.csv"), std::size_t{1});

            // CTRL+S writes channels/unitmaps/generic.csv.json.
            w->returnFocus();
            uitest::press(*w, Qt::Key_S, Qt::ControlModifier);
            const std::filesystem::path file = p.folder / "channels" / "unitmaps" / "generic.csv.json";
            QVERIFY(std::filesystem::exists(file));
            std::vector<std::string> log;
            const Result<formats::UnitMappingProfile> saved = formats::UnitMappingProfile::load(file, log);
            QVERIFY(saved.ok());
            QCOMPARE(saved->rules.size(), std::size_t{2});

            // A complete unit map applies silently.
            const int steps = w->session().undoStack().count();
            QSignalSpy shown(&panel, &MappingPanel::pendingChanged);
            w->openLog(writeCsv(p.logs, "next.csv"));
            QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(*w, "next.csv") != nullptr, 30000);
            QCOMPARE(shown.count(), 0);
            QVERIFY(!panel.isVisible());
            QCOMPARE(w->session().undoStack().count(), steps);
        }
        rt->shutdown();
    }

    void saveToUnitMapThenOpenAnyway() {
        auto rt = uitest::makeRuntime();
        Project p;
        QVERIFY(p.prepare(*rt));
        {
            auto w = shell(*rt, p);
            QVERIFY(w != nullptr);
            MappingPanel& panel = w->mappingPanel();
            w->openLog(writeCsv(p.logs, "first.csv"));
            QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
            QVERIFY(panel.isVisible());
            const int boost = rowOf(panel, "Boost");
            QVERIFY(panel.setUnit(boost, QStringLiteral("bar")));
            QString why;
            QVERIFY(panel.save(&why)); // the complete row into the project's unit map; the open stays pending
            QVERIFY(panel.blocksPublish());
            QCOMPARE(w->session().undoStack().undoText(), QStringLiteral("Save unit map"));
            QCOMPARE(w->session().unitMaps().at("generic.csv").rules.size(), std::size_t{1});
            QTest::qWait(100);
            QVERIFY(sourceNamed(*w, "first.csv") == nullptr);
            panel.openAnyway(); // Coolant stays unitless
            QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(*w, "first.csv") != nullptr, 30000);
            QTRY_VERIFY(!panel.isVisible());
            const auto first = sourceNamed(*w, "first.csv");
            QCOMPARE(qt::test::shippedUnits()->quantity(channel(*first, "Boost")->desc.quantity).name,
                     std::string("Pressure"));

            // The next file still lacks Coolant: the panel asks for that row only.
            w->openLog(writeCsv(p.logs, "second.csv"));
            QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
            QCOMPARE(panel.rows().size(), std::size_t{1});
            QCOMPARE(panel.rows()[0].loggedName, std::string("Coolant"));
            panel.cancel();
            QTRY_VERIFY(!panel.isVisible());
            QTest::qWait(100);
            QVERIFY(sourceNamed(*w, "second.csv") == nullptr);
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstMappingPanel)
#include "tst_mapping_panel.moc"
