// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DetailsEditor (06 section 13.4; D-015, D-032), offscreen over two written logs opened through the real
// loader: one column per source and the profile's details first (derived File details left out); typed edits
// through the grid's model as one SetDetailCommand each into the source's sidecar (provenance User, the
// original kept, a dot and a tool tip on the cell); text may be empty; numbers keep their digits and take the
// profile's unit when none is typed, a unit of another quantity or an unknown one is refused with the reason;
// date-times parse ISO 8601; Restore Original reverts one cell; undo and redo; a Venue rename is announced;
// the rows sort by name and come back to the profile order; Copy as TSV.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/editors_data/details_editor.hpp>

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QPushButton>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <memory>

// The qtmodel synthetic-source helpers, compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::makeSource;
using esses::qt::test::repoRoot;
using esses::qt::test::scratchDir;
using esses::qt::test::shippedUnits;
using esses::qt::test::SyntheticOptions;
using esses::qt::test::writeLd;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

namespace {

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

struct Rig {
    std::filesystem::path root; // first: the scratch directory is made once
    qt::AppRuntime rt;
    qt::ProjectSession session;
    qt::SourceLoader loader;
    qt::DataSelectionModel selection;
    std::shared_ptr<const model::DataSource> a;
    std::shared_ptr<const model::DataSource> b;

    explicit Rig(const std::string& name)
        : root(scratchDir(name)), rt([] {
              qt::AppRuntimeOptions o;
              o.workers = 2;
              o.cacheBudget = std::size_t{128} << 20;
              o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
              return o;
          }()),
          session(pathsFor(root / "config")), loader(rt.pool(), rt.dispatcher(), shippedUnits()),
          selection(&session) {
        static_cast<void>(session.loadDefaults());
        loader.setUseSidecars(false);
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        qt::connectSourceLoader(loader, selection, nullptr);
        const std::filesystem::path dir = root / "logs";
        std::filesystem::create_directories(dir);
        a = open(dir, "details-a", {40.0, 50.0, 30.0});
        b = open(dir, "details-b", {35.0, 52.0, 31.0}); // other content: another source (its quick hash)
    }

    std::shared_ptr<const model::DataSource> open(const std::filesystem::path& dir, const std::string& stem,
                                                  std::vector<double> lapSeconds) {
        SyntheticOptions o;
        o.path = stem + ".ld";
        o.lapSeconds = std::move(lapSeconds);
        std::filesystem::path ld;
        {
            ThreadRole::Scope normal(ThreadRole::Normal); // writing the log waits on purpose
            ld = writeLd(*makeSource(o, rt.cache()), dir, stem);
        }
        QSignalSpy published(&loader, &qt::SourceLoader::sourcePublished);
        loader.open(ld);
        if (!QTest::qWaitFor([&] { return published.count() == 1; }, 30000)) {
            qFatal("open %s failed", stem.c_str());
        }
        return published.first().at(0).value<std::shared_ptr<const model::DataSource>>();
    }
    QUndoStack& undo() { return session.undoStack(); }
};

QModelIndex cellIndex(DetailsEditor& e, const QString& id, int sourceIndex) {
    QAbstractItemModel& m = e.viewModel();
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.index(r, 0).data().toString() == id) {
            return m.index(r, sourceIndex + 1);
        }
    }
    return {};
}

} // namespace

class TstDetailsEditor : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    void parsesTypedText() {
        const auto& units = *shippedUnits();
        QString why;
        auto v = DetailsEditor::parseValue(model::DetailType::Text, QString(), "", &units, &why);
        QVERIFY(v.has_value());
        QCOMPARE(std::get<std::string>(*v), std::string()); // empty strings are allowed
        v = DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral(" 4.0047 km "), "m", &units,
                                      &why);
        QVERIFY(v.has_value());
        const auto& q = std::get<model::Quantityd>(*v);
        QCOMPARE(q.v, 4.0047);
        QCOMPARE(q.dps, 4);
        QCOMPARE(q.symbol, std::string("km"));
        QCOMPARE(q.unit, *units.find("km"));
        v = DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral("1250"), "kg", &units, &why);
        QCOMPARE(std::get<model::Quantityd>(*v).symbol, std::string("kg"));
        QCOMPARE(std::get<model::Quantityd>(*v).dps, 0);
        v = DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral("-.5"), "", &units, &why);
        QCOMPARE(std::get<model::Quantityd>(*v).v, -0.5);
        QVERIFY(!DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral("3 kg"), "km", &units,
                                           &why));
        QVERIFY(why.contains(QStringLiteral("Distance")));
        QVERIFY(!DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral("3 zorks"), "", &units,
                                           &why));
        QVERIFY(why.contains(QStringLiteral("zorks")));
        QVERIFY(!DetailsEditor::parseValue(model::DetailType::Number, QString(), "km", &units, &why));
        QVERIFY(
            !DetailsEditor::parseValue(model::DetailType::Number, QStringLiteral("abc"), "", &units, &why));
        v = DetailsEditor::parseValue(model::DetailType::DateTime,
                                      QStringLiteral("2026-10-12T14:05:00+02:00"), "", &units, &why);
        QVERIFY(v.has_value());
        QCOMPARE(std::get<model::DateTime>(*v).tzOffsetMin, 120);
        QVERIFY(!DetailsEditor::parseValue(model::DetailType::DateTime, QStringLiteral("yesterday"), "",
                                           &units, &why));
        // The edit text keeps every significant digit (numbers are not rounded on reopen).
        model::Quantityd third;
        third.v = 1.0 / 3.0;
        third.symbol = "m";
        QCOMPARE(DetailsEditor::valueText(third), QStringLiteral("0.3333333333333333 m"));
    }

    void gridEditsRestoreAndUndo() {
        Rig rig("de-details-edit");
        DetailsEditor e(rig.session, rig.loader);
        QVERIFY(!e.isModal());
        QCOMPARE(e.sourceCount(), 2);
        const QStringList ids = e.detailIds();
        QCOMPARE(ids.mid(0, 4), (QStringList{QStringLiteral("Driver"), QStringLiteral("Venue"),
                                             QStringLiteral("Venue Length"), QStringLiteral("Start Lap")}));
        QVERIFY(!ids.contains(QStringLiteral("File Name"))); // derived details are not edited
        QCOMPARE(e.viewModel().headerData(1, Qt::Horizontal).toString(), QStringLiteral("details-a.ld"));
        QCOMPARE(e.viewModel().headerData(2, Qt::Horizontal).toString(), QStringLiteral("details-b.ld"));

        // A text edit through the grid's model (the double-click editor writes the same way).
        const QString before = e.cellText(QStringLiteral("Driver"), 0);
        QVERIFY(e.viewModel().setData(cellIndex(e, QStringLiteral("Driver"), 0), QStringLiteral("Ada")));
        QCOMPARE(rig.undo().count(), 1);
        QCOMPARE(rig.undo().undoText(), QStringLiteral("Edit detail Driver"));
        QCOMPARE(e.cellText(QStringLiteral("Driver"), 0), QStringLiteral("Ada"));
        QVERIFY(e.isEdited(QStringLiteral("Driver"), 0));
        const QModelIndex driver = cellIndex(e, QStringLiteral("Driver"), 0);
        QVERIFY(driver.data(DetailsEditor::EditedRole).toBool());
        QVERIFY(driver.data(Qt::DecorationRole).isValid()); // the dot
        QVERIFY(driver.data(Qt::ToolTipRole).toString().startsWith(QStringLiteral("Edited")));
        // The sidecar holds the value with provenance User and the original.
        const auto src = rig.loader.source(rig.a->id);
        const model::Detail* stored = model::findDetail(src->sidecar.layer("Normal").details, "Driver");
        QVERIFY(stored != nullptr);
        QCOMPARE(stored->prov, model::Provenance::User);
        QVERIFY(!e.setCell(QStringLiteral("Driver"), 0, QStringLiteral("Ada"))); // unchanged
        QCOMPARE(rig.undo().count(), 1);
        // An empty string is a value too.
        QVERIFY(e.setCell(QStringLiteral("Driver"), 1, QString()));
        QVERIFY(e.isEdited(QStringLiteral("Driver"), 1));
        QCOMPARE(e.cellText(QStringLiteral("Driver"), 1), QString());

        // Numbers: the profile's unit when none is typed, the digits kept on reopen.
        QVERIFY(e.setCell(QStringLiteral("Venue Length"), 0, QStringLiteral("4.0047 km")));
        QCOMPARE(e.cellText(QStringLiteral("Venue Length"), 0), QStringLiteral("4.0047 km"));
        DetailsEditor reopened(rig.session, rig.loader);
        QCOMPARE(reopened.cellText(QStringLiteral("Venue Length"), 0), QStringLiteral("4.0047 km"));
        QVERIFY(e.setCell(QStringLiteral("Vehicle Weight"), 1, QStringLiteral("1250")));
        QCOMPARE(e.cellText(QStringLiteral("Vehicle Weight"), 1), QStringLiteral("1250 kg"));
        QVERIFY(!e.setCell(QStringLiteral("Venue Length"), 0, QStringLiteral("3 kg")));
        QVERIFY(e.lastError().contains(QStringLiteral("kg")));
        QVERIFY(
            !e.viewModel().setData(cellIndex(e, QStringLiteral("Venue Length"), 1), QStringLiteral("fast")));
        QCOMPARE(rig.undo().count(), 4);

        // Restore Original reverts one cell; undo brings the edit back.
        e.grid().setCurrentIndex(cellIndex(e, QStringLiteral("Driver"), 0));
        auto* restore = e.findChild<QPushButton*>(QStringLiteral("restoreOriginal"));
        QVERIFY(restore->isEnabled());
        restore->click();
        QCOMPARE(rig.undo().count(), 5);
        QVERIFY(!e.isEdited(QStringLiteral("Driver"), 0));
        QCOMPARE(e.cellText(QStringLiteral("Driver"), 0), before);
        QVERIFY(!e.restoreOriginal(QStringLiteral("Driver"), 0)); // the file's value already
        rig.undo().undo();
        QCOMPARE(e.cellText(QStringLiteral("Driver"), 0), QStringLiteral("Ada"));
        rig.undo().redo();
        QCOMPARE(e.cellText(QStringLiteral("Driver"), 0), before);
    }

    void venueRenameSortAndTsv() {
        Rig rig("de-details-sort");
        DetailsEditor e(rig.session, rig.loader);
        QSignalSpy renamed(&e, &DetailsEditor::venueRenamed);
        QVERIFY(e.setCell(QStringLiteral("Venue"), 1, QStringLiteral("Spa")));
        QCOMPARE(renamed.count(), 1);
        QVERIFY(renamed.last().at(0).value<model::SourceId>() == rig.b->id);
        QCOMPARE(renamed.last().at(2).toString(), QStringLiteral("Spa"));

        // Copy as TSV: a header line, then one line per detail in the shown order.
        const QString tsv = e.tableText();
        const QStringList lines = tsv.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.first(), QStringLiteral("Detail\tdetails-a.ld\tdetails-b.ld"));
        QCOMPARE(lines.size(), e.detailIds().size() + 1);
        QVERIFY(lines.at(1).startsWith(QStringLiteral("Driver\t")));
        QVERIFY(lines.at(2).endsWith(QStringLiteral("\tSpa")));
        e.copyAsTsv();
        QCOMPARE(QApplication::clipboard()->text(), tsv);

        // Sorting by the Detail column, then back to the profile order.
        e.grid().sortByColumn(0, Qt::AscendingOrder);
        QStringList sorted = e.detailIds();
        std::sort(sorted.begin(), sorted.end(),
                  [](const QString& x, const QString& y) { return x.compare(y, Qt::CaseInsensitive) < 0; });
        QCOMPARE(e.viewModel().index(0, 0).data().toString(), sorted.first());
        QCOMPARE(e.viewModel().index(e.viewModel().rowCount() - 1, 0).data().toString(), sorted.last());
        e.grid().horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
        QCOMPARE(e.viewModel().index(0, 0).data().toString(), QStringLiteral("Driver"));

        // A closed source leaves the grid.
        rig.loader.close(rig.a->id);
        QCOMPARE(e.sourceCount(), 1);
        QCOMPARE(e.viewModel().headerData(1, Qt::Horizontal).toString(), QStringLiteral("details-b.ld"));
    }
};

QTEST_MAIN(TstDetailsEditor)
#include "tst_details_editor.moc"
