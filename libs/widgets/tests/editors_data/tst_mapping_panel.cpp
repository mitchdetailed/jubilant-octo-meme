// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MappingPanel (05 section 8.1, 06 section 13.8; AC-M2-11), offscreen over the real open pipeline: with the
// hand-off on, a CSV with two unknown units makes the panel appear by itself and blocks publication (Resolve
// refuses while a row lacks a quantity and unit; nothing publishes and no worker waits); the rows are edited
// through the table model and its combo-box editors (units filtered by the quantity, a unit first sets the
// quantity, a wrong pair is refused); Resolve records one undoable "Resolve mapping" (the ticked rows in the
// project's unit map, the analysis name as a project Mapping) and the file publishes with the units; the unit
// map is written to channels/unitmaps/generic.csv.json by the project's save; a second file asks only for
// what the map lacks, Save completes the map without opening, and a third file opens silently; Scan picks up
// a map saved meanwhile; pending opens queue behind the shown one; Cancel and Open Anyway; undo removes the
// decision.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/editors_data/mapping_panel.hpp>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTest>
#include <QUndoStack>
#include <fstream>
#include <memory>

// The qtmodel synthetic-source helpers, compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::repoRoot;
using esses::qt::test::scratchDir;
using esses::qt::test::shippedUnits;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)

namespace {

// Two unknown units (zorks, blips) and one the units table knows.
constexpr std::string_view kCsv = "Time,Boost,Coolant,Speed\n"
                                  "s,zorks,blips,km/h\n"
                                  "0,100,20,36\n"
                                  "0.5,150,21,72\n"
                                  "1.0,200,22,108\n"
                                  "1.5,210,23,110\n";

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

// Each file gets a last row of its own: identical content would be the same source (its quick hash).
std::filesystem::path writeCsv(const std::filesystem::path& dir, const std::string& name) {
    static int serial = 0;
    const std::filesystem::path p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out << kCsv << "2.0," << 220 + (++serial) << ",24,111\n";
    return p;
}

const model::Channel* channel(const model::DataSource& src, std::string_view name) {
    const model::DataLayer* l = src.layer(model::kNormalLayer);
    return l == nullptr ? nullptr : l->find(name);
}

bool logged(const QSignalSpy& spy, std::string_view code) {
    for (const QList<QVariant>& args : spy) {
        for (const formats::ImportLogEntry& e : args.at(1).value<std::vector<formats::ImportLogEntry>>()) {
            if (e.code == code) {
                return true;
            }
        }
    }
    return false;
}

struct Workspace {
    qt::AppRuntime rt;
    qt::ProjectSession session;
    qt::MathsService maths;
    qt::SourceLoader loader;
    qt::DataSelectionModel selection;
    std::unique_ptr<MappingPanel> panel;

    explicit Workspace(const std::filesystem::path& dir)
        : rt([] {
              qt::AppRuntimeOptions o;
              o.workers = 4;
              o.cacheBudget = std::size_t{128} << 20;
              o.markGuiThreadNoBlock = true; // as esses (AC-M1-12): nothing here may wait on the GUI thread
              return o;
          }()),
          session(pathsFor(dir / "config")), maths(rt.pool(), rt.dispatcher(), shippedUnits()),
          loader(rt.pool(), rt.dispatcher(), shippedUnits()), selection(&session) {
        if (!session.newProject(dir / "project", "Mapping").ok()) {
            qFatal("newProject failed");
        }
        loader.setUseSidecars(false);
        loader.setMappingHandOff(true);
        loader.setMathsService(&maths);
        qt::connectSourceLoader(loader, selection, nullptr);
        qt::connectProjectSession(session, &maths, &loader);
        panel = std::make_unique<MappingPanel>(loader, session, shippedUnits());
    }
    ~Workspace() { panel.reset(); }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    QAbstractItemModel& rows() { return *panel->table().model(); }
    QModelIndex cell(int row, int column) { return rows().index(row, column); }
    QString text(int row, int column) { return cell(row, column).data().toString(); }
    bool edit(int row, int column, const QString& value) { return rows().setData(cell(row, column), value); }
};

int rowOf(MappingPanel& panel, const char* loggedName) {
    for (int i = 0; i < static_cast<int>(panel.rows().size()); ++i) {
        if (panel.rows()[static_cast<std::size_t>(i)].loggedName == loggedName) {
            return i;
        }
    }
    return -1;
}

} // namespace

class TstMappingPanel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
    }

    // AC-M2-11: unknown units block publication until resolved; the decision is undoable and saved for later
    // files; a complete unit map applies silently.
    void unknownUnitsBlockUntilResolved() {
        const auto dir = scratchDir("de-mapping-resolve");
        Workspace w(dir);
        MappingPanel& panel = *w.panel;
        QVERIFY(!panel.blocksPublish());
        QSignalSpy published(&w.loader, &qt::SourceLoader::sourcePublished);
        QSignalSpy pendingChanged(&panel, &MappingPanel::pendingChanged);
        w.loader.open(writeCsv(dir, "b.csv"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000); // the panel follows mappingRequired itself
        QCOMPARE(pendingChanged.count(), 1);
        QCOMPARE(pendingChanged.last().at(0).toBool(), true);
        QCOMPARE(w.loader.pendingOpens(), 0); // task A ended: no worker waits for the panel
        QCOMPARE(panel.importerStem(), std::string("generic.csv"));
        QCOMPARE(w.rows().rowCount(), 2);
        QCOMPARE(w.rows().columnCount(), int(MappingPanel::ColumnCount));
        const int boost = rowOf(panel, "Boost");
        const int coolant = rowOf(panel, "Coolant");
        QVERIFY(boost >= 0 && coolant >= 0);
        QCOMPARE(w.text(boost, MappingPanel::VendorUnitCol), QStringLiteral("zorks"));
        QCOMPARE(w.text(coolant, MappingPanel::VendorUnitCol), QStringLiteral("blips"));
        QVERIFY(w.text(boost, MappingPanel::SamplesCol).startsWith(QStringLiteral("100, 150")));
        QCOMPARE(w.text(boost, MappingPanel::RuleCol), QStringLiteral("Name and unit"));
        QCOMPARE(w.cell(boost, MappingPanel::FutureCol).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QVERIFY(w.text(boost, MappingPanel::StatusCol).contains(QStringLiteral("quantity")));
        QVERIFY(panel.bar().text().contains(QStringLiteral("b.csv")));
        QVERIFY(panel.bar().text().contains(QStringLiteral("2 channels")));

        // Resolve refuses while a row lacks its quantity and unit; nothing publishes.
        QVERIFY(!panel.resolve());
        QVERIFY(panel.lastError().contains(QStringLiteral("Boost")) ||
                panel.lastError().contains(QStringLiteral("Coolant")));
        QVERIFY(panel.blocksPublish());
        QCOMPARE(w.session.undoStack().count(), 0);
        QTest::qWait(100);
        QCOMPARE(published.count(), 0);

        // Quantity first: the unit choices follow it; a unit of another quantity is refused.
        QVERIFY(w.edit(boost, MappingPanel::QuantityCol, QStringLiteral("Pressure")));
        const QStringList pressureUnits =
            w.cell(boost, MappingPanel::UnitCol).data(MappingPanel::ChoicesRole).toStringList();
        QVERIFY(pressureUnits.contains(QStringLiteral("kPa a")));
        QVERIFY(!pressureUnits.contains(QStringLiteral("deg C")));
        QVERIFY(!w.edit(boost, MappingPanel::UnitCol, QStringLiteral("deg C")));
        QVERIFY(panel.lastError().contains(QStringLiteral("Pressure")));
        QVERIFY(!w.edit(boost, MappingPanel::QuantityCol, QStringLiteral("No Such Quantity")));
        QVERIFY(w.edit(boost, MappingPanel::UnitCol, QStringLiteral("kPa"))); // an alias: the symbol is kept
        QCOMPARE(w.text(boost, MappingPanel::UnitCol), QStringLiteral("kPa a"));
        QVERIFY(panel.rows()[static_cast<std::size_t>(boost)].complete());
        QVERIFY(w.edit(boost, MappingPanel::AnalysisNameCol, QStringLiteral("Boost Pressure")));
        // Unit first: it sets the quantity. This row applies to this file only.
        QVERIFY(w.edit(coolant, MappingPanel::UnitCol, QStringLiteral("deg C")));
        QCOMPARE(w.text(coolant, MappingPanel::QuantityCol), QStringLiteral("Temperature"));
        QVERIFY(
            w.rows().setData(w.cell(coolant, MappingPanel::FutureCol), Qt::Unchecked, Qt::CheckStateRole));
        QVERIFY(panel.bar().text().contains(QStringLiteral("every channel")));

        QVERIFY(panel.resolve());
        QVERIFY(!panel.blocksPublish());
        QCOMPARE(pendingChanged.last().at(0).toBool(), false);
        QCOMPARE(w.session.undoStack().count(), 1);
        QCOMPARE(w.session.undoStack().undoText(), QStringLiteral("Resolve mapping"));
        QCOMPARE(w.session.unitMaps().count("generic.csv"), std::size_t{1});
        const formats::UnitMappingProfile& map = w.session.unitMaps().at("generic.csv");
        QCOMPARE(map.rules.size(), std::size_t{1}); // the ticked row only
        QCOMPARE(map.rules[0].name, std::string("Boost"));
        QCOMPARE(map.rules[0].vendorUnit, std::string("zorks"));
        QCOMPARE(map.rules[0].unit, std::string("kPa a"));
        QCOMPARE(map.importer, std::string(formats::kGenericCsvImporterId));
        QCOMPARE(w.session.mappings().rows.size(), std::size_t{1});
        QCOMPARE(w.session.mappings().rows[0].to, std::string("Boost Pressure"));
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        auto src = published.last().at(0).value<std::shared_ptr<const model::DataSource>>();
        const model::Channel* boosted = channel(*src, "Boost Pressure");
        QVERIFY(boosted != nullptr);
        QCOMPARE(shippedUnits()->quantity(boosted->desc.quantity).name, std::string("Pressure"));
        const model::Channel* cool = channel(*src, "Coolant");
        QVERIFY(cool != nullptr);
        QCOMPARE(shippedUnits()->quantity(cool->desc.quantity).name, std::string("Temperature"));
        QVERIFY(cool->desc.statusText.empty());

        // The project's save writes channels/unitmaps/generic.csv.json.
        QVERIFY(w.session.save().ok());
        const std::filesystem::path file = dir / "project" / "channels" / "unitmaps" / "generic.csv.json";
        QVERIFY(std::filesystem::exists(file));
        std::vector<std::string> log;
        const auto saved = formats::UnitMappingProfile::load(file, log);
        QVERIFY(saved.ok());
        QCOMPARE(saved->rules.size(), std::size_t{1});

        // A second file asks only for what the map lacks (Coolant); Save completes the map without opening.
        w.loader.open(writeCsv(dir, "c.csv"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
        QCOMPARE(w.rows().rowCount(), 1);
        QCOMPARE(panel.rows()[0].loggedName, std::string("Coolant"));
        QString why;
        QVERIFY(!panel.save(&why)); // nothing complete yet
        QVERIFY(!why.isEmpty());
        QVERIFY(w.edit(0, MappingPanel::UnitCol, QStringLiteral("deg C")));
        QSignalSpy hints(&panel, &MappingPanel::hint);
        QVERIFY(panel.save(&why));
        QVERIFY(why.isEmpty());
        QCOMPARE(hints.count(), 1);
        QCOMPARE(w.session.unitMaps().at("generic.csv").rules.size(), std::size_t{2});
        QCOMPARE(w.session.undoStack().undoText(), QStringLiteral("Save unit map"));
        QVERIFY(panel.blocksPublish()); // Save does not open (06 section 13.8: Resolve or Open Anyway does)
        QCOMPARE(w.text(0, MappingPanel::StatusCol), QStringLiteral("Resolved (unit map)"));
        QTest::qWait(50);
        QCOMPARE(published.count(), 1);
        QVERIFY(panel.resolve());
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 2, 30000);
        QCOMPARE(w.session.undoStack().count(), 2); // nothing new to record for the second file

        // A complete unit map applies silently: the panel never appears.
        pendingChanged.clear();
        w.loader.open(writeCsv(dir, "d.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 3, 30000);
        QCOMPARE(pendingChanged.count(), 0);
        QVERIFY(!panel.blocksPublish());
        src = published.last().at(0).value<std::shared_ptr<const model::DataSource>>();
        QVERIFY(channel(*src, "Boost Pressure") != nullptr);
        QVERIFY(channel(*src, "Coolant")->desc.statusText.empty());

        // Undo takes the decisions back: the map is gone and the next file asks again.
        w.session.undoStack().undo();
        w.session.undoStack().undo();
        QCOMPARE(w.session.unitMaps().count("generic.csv"), std::size_t{0});
        QVERIFY(w.session.mappings().rows.empty());
        w.loader.open(writeCsv(dir, "e.csv"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
        QCOMPARE(w.rows().rowCount(), 2);
        panel.cancel();
    }

    // The combo-box editors of the Quantity, Unit and Rule cells.
    void cellEditorsAreCombos() {
        const auto dir = scratchDir("de-mapping-editors");
        Workspace w(dir);
        MappingPanel& panel = *w.panel;
        panel.resize(900, 300);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        w.loader.open(writeCsv(dir, "f.csv"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
        QAbstractItemDelegate* delegate = panel.table().itemDelegate();
        QStyleOptionViewItem option;
        const QModelIndex quantity = w.cell(0, MappingPanel::QuantityCol);
        std::unique_ptr<QWidget> editor(delegate->createEditor(panel.table().viewport(), option, quantity));
        auto* combo = qobject_cast<QComboBox*>(editor.get());
        QVERIFY(combo != nullptr);
        QVERIFY(combo->findText(QStringLiteral("Pressure")) >= 0);
        QCOMPARE(combo->findText(QStringLiteral("Unitless")), -1);
        combo->setCurrentIndex(combo->findText(QStringLiteral("Pressure")));
        delegate->setModelData(combo, &w.rows(), quantity);
        QCOMPARE(w.text(0, MappingPanel::QuantityCol), QStringLiteral("Pressure"));
        const QModelIndex rule = w.cell(0, MappingPanel::RuleCol);
        editor.reset(delegate->createEditor(panel.table().viewport(), option, rule));
        combo = qobject_cast<QComboBox*>(editor.get());
        QVERIFY(combo != nullptr);
        QCOMPARE(combo->count(), 3); // name and unit, name only, unit only
        combo->setCurrentIndex(combo->findText(QStringLiteral("Unit only")));
        delegate->setModelData(combo, &w.rows(), rule);
        QCOMPARE(panel.rows()[0].match, formats::UnitMappingProfile::Match::UnitOnly);
        // The analysis name is a plain text editor.
        editor.reset(delegate->createEditor(panel.table().viewport(), option,
                                            w.cell(0, MappingPanel::AnalysisNameCol)));
        QVERIFY(qobject_cast<QComboBox*>(editor.get()) == nullptr);
        editor.reset();
        // A unitOnly rule is saved without a name and resolves the unit for every channel.
        QVERIFY(w.edit(0, MappingPanel::UnitCol, QStringLiteral("kPa")));
        QVERIFY(w.edit(1, MappingPanel::UnitCol, QStringLiteral("deg C")));
        QVERIFY(panel.resolve());
        const auto& rules = w.session.unitMaps().at("generic.csv").rules;
        const auto unitOnly = std::find_if(rules.begin(), rules.end(), [](const auto& r) {
            return r.match == formats::UnitMappingProfile::Match::UnitOnly;
        });
        QVERIFY(unitOnly != rules.end());
        QVERIFY(unitOnly->name.empty());
        QCOMPARE(unitOnly->vendorUnit, std::string("zorks"));
        // The buttons are disabled while nothing waits.
        QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("mappingResolve"))->isEnabled());
    }

    // Scan reads the project's unit map anew (a decision taken meanwhile); Resolve then records nothing new.
    void scanPicksUpAMapSavedMeanwhile() {
        const auto dir = scratchDir("de-mapping-scan");
        Workspace w(dir);
        MappingPanel& panel = *w.panel;
        w.loader.open(writeCsv(dir, "g.csv"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), 30000);
        QVERIFY(!panel.rows()[0].complete());
        formats::UnitMappingProfile map;
        map.importer = std::string(formats::kGenericCsvImporterId);
        map.rules.push_back({formats::UnitMappingProfile::Match::NameAndUnit, "Boost", "zorks", "Pressure",
                             "kPa", "Boost Abs"});
        map.rules.push_back(
            {formats::UnitMappingProfile::Match::UnitOnly, "", "blips", "Temperature", "deg C", ""});
        w.session.undoStack().push(new qt::SetUnitMapCommand(w.session, "generic.csv", map));
        panel.findChild<QPushButton*>(QStringLiteral("mappingScan"))->click();
        for (const MappingPanel::Row& r : panel.rows()) {
            QVERIFY(r.complete());
            QCOMPARE(r.origin, QStringLiteral("unit map"));
        }
        QCOMPARE(panel.rows()[static_cast<std::size_t>(rowOf(panel, "Boost"))].analysisName,
                 std::string("Boost Abs"));
        QCOMPARE(panel.rows()[static_cast<std::size_t>(rowOf(panel, "Coolant"))].match,
                 formats::UnitMappingProfile::Match::UnitOnly);
        QSignalSpy published(&w.loader, &qt::SourceLoader::sourcePublished);
        panel.findChild<QPushButton*>(QStringLiteral("mappingResolve"))->click();
        QVERIFY(!panel.blocksPublish());
        // The map already holds the rules: only the analysis name is new (a project Mapping).
        QCOMPARE(w.session.undoStack().count(), 2);
        QCOMPARE(w.session.unitMaps().at("generic.csv").rules.size(), std::size_t{2});
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        const auto src = published.last().at(0).value<std::shared_ptr<const model::DataSource>>();
        QVERIFY(channel(*src, "Boost Abs") != nullptr);
    }

    // Several pending opens queue behind the shown one; Cancel drops the open (Import Log), Open Anyway opens
    // unitless and records nothing.
    void queueCancelAndOpenAnyway() {
        const auto dir = scratchDir("de-mapping-queue");
        Workspace w(dir);
        MappingPanel& panel = *w.panel;
        QSignalSpy published(&w.loader, &qt::SourceLoader::sourcePublished);
        QSignalSpy failed(&w.loader, &qt::SourceLoader::openFailed);
        QSignalSpy logs(&w.loader, &qt::SourceLoader::importLog);
        QSignalSpy required(&w.loader, &qt::SourceLoader::mappingRequired);
        w.loader.open(writeCsv(dir, "h.csv"));
        w.loader.open(writeCsv(dir, "i.csv"));
        QTRY_COMPARE_WITH_TIMEOUT(required.count(), 2, 30000);
        QVERIFY(panel.blocksPublish());
        QCOMPARE(panel.queued(), 1);
        QVERIFY(panel.bar().text().contains(QStringLiteral("1 more file")));
        const int first = panel.pendingId();
        const std::string firstPath = panel.pending().path; // the two opens race: either may come first
        // The same id again (a caller driving the panel too) is ignored.
        panel.showPending(first, *w.loader.pendingMapping(first));
        QCOMPARE(panel.queued(), 1);
        panel.findChild<QPushButton*>(QStringLiteral("mappingCancel"))->click();
        QVERIFY(panel.blocksPublish()); // the queued open is shown now
        QVERIFY(panel.pendingId() != first);
        const std::string secondPath = panel.pending().path;
        QVERIFY(secondPath != firstPath);
        QCOMPARE(panel.queued(), 0);
        QCOMPARE(w.loader.pendingMappings(), 1);
        QVERIFY(logged(logs, "open.mapping_cancelled"));
        panel.findChild<QPushButton*>(QStringLiteral("mappingOpenAnyway"))->click();
        QVERIFY(!panel.blocksPublish());
        QCOMPARE(w.session.undoStack().count(), 0); // nothing to record
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QCOMPARE(failed.count(), 0);
        const auto src = published.last().at(0).value<std::shared_ptr<const model::DataSource>>();
        QVERIFY(src->prov.path.filename() == std::filesystem::path(secondPath).filename());
        QVERIFY(!channel(*src, "Boost")->desc.statusText.empty()); // unitless with its status text
        QVERIFY(!panel.resolve());                                 // nothing waits
        QVERIFY(!panel.save());
    }
};

QTEST_MAIN(TstMappingPanel)
#include "tst_mapping_panel.moc"
