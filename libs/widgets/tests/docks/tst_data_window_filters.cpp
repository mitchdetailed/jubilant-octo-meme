// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Data-window filters (06 sections 5.4 and 6; D-162), offscreen: the Filters submenu of a row's source sets
// and checks every kind (All, Selected, Fastest, Fastest 3, Within 1 % of Fastest, Lap Numbers...) and the
// tree shows the filtered ranges, slots always kept with the filter icon and their tool tip, the filtered
// source row with the icon and the filter's name; the lap-number editor shows a parse error as you type and,
// on Apply, keeps the previous filter and stays open, else applies and closes, and comes back prefilled;
// Apply Filter To All Sources (a local action, or the shared data.filter_all_sources action with
// CommandActions); with a session the filters are written to project.json dataWindow and never to the undo
// stack.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAction>
#include <QApplication>
#include <QIdentityProxyModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>
#include <memory>

// The qtmodel synthetic-source helpers (in memory, circuit laps), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::makeSource;
using esses::qt::test::rangeRef;
using esses::qt::test::repoRoot;
using esses::qt::test::scratchDir;
using esses::qt::test::SyntheticOptions;
using K = esses::qt::FilterSpec::Kind;

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
    std::filesystem::path root;
    qt::AppRuntime rt{[] {
        qt::AppRuntimeOptions o;
        o.workers = 2;
        o.cacheBudget = std::size_t{64} << 20;
        o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
        return o;
    }()};
    qt::ProjectSession session;
    qt::DataSelectionModel selection;
    qt::CommandRegistry commands;
    std::unique_ptr<DataWindow> data;
    std::shared_ptr<const model::DataSource> a;
    std::shared_ptr<const model::DataSource> b;

    explicit Rig(const std::string& name)
        : root(scratchDir(name)), session(pathsFor(root / "config")), selection(&session) {
        static_cast<void>(session.loadDefaults());
        // Out 40 s, laps 1-3 of 50, 48 and 49 s, in lap (number 4) 30 s.
        SyntheticOptions oa;
        oa.path = name + "-a.ld";
        oa.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
        a = makeSource(oa, rt.cache());
        SyntheticOptions ob;
        ob.path = name + "-b.ld";
        ob.lapSeconds = {30.0, 45.0, 44.0, 46.0, 20.0};
        b = makeSource(ob, rt.cache());
        data = std::make_unique<DataWindow>(selection, commands);
        selection.sourcePublished(a);
        selection.sourcePublished(b);
        data->resize(700, 500);
    }

    QAbstractItemModel& view() { return data->viewModel(); }
    // The Name cell of a source row (children hang off column 0: parentOf).
    QModelIndex sourceRow(int i) { return view().index(i, qt::DataWindowModel::NameCol); }
    QModelIndex parentOf(int i) { return view().index(i, 0); }
    int rows(int source) { return view().rowCount(parentOf(source)); }
    QStringList names(int source) {
        QStringList out;
        for (int r = 0; r < rows(source); ++r) {
            out << view().index(r, qt::DataWindowModel::NameCol, parentOf(source)).data().toString();
        }
        return out;
    }
    QModelIndex range(int source, const QString& name) {
        for (int r = 0; r < rows(source); ++r) {
            const QModelIndex i = view().index(r, qt::DataWindowModel::NameCol, parentOf(source));
            if (i.data().toString() == name) {
                return i;
            }
        }
        return {};
    }
};

QMenu* filtersOf(QMenu& menu) {
    for (QAction* a : menu.actions()) {
        if (a->menu() != nullptr && a->menu()->objectName() == QStringLiteral("dataFilters")) {
            return a->menu();
        }
    }
    return nullptr;
}

QAction* actionOf(QMenu& menu, const QString& text) {
    for (QAction* a : menu.actions()) {
        if (a->text() == text) {
            return a;
        }
    }
    return nullptr;
}

// Builds the row's context menu and triggers the Filters entry `text`.
void pick(Rig& rig, const QModelIndex& row, const QString& text) {
    const std::unique_ptr<QMenu> menu = rig.data->contextMenu(row);
    QVERIFY(menu != nullptr);
    QMenu* filters = filtersOf(*menu);
    QVERIFY(filters != nullptr);
    QAction* a = actionOf(*filters, text);
    QVERIFY2(a != nullptr, qPrintable(text));
    a->trigger();
}

} // namespace

class TstDataWindowFilters : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void filtersMenuSetsEveryKind() {
        Rig rig("de-dwf-kinds");
        QCOMPARE(rig.rows(0), 5);
        QCOMPARE(rig.selection.snapshot().main.src, rig.a->id); // Main = the out lap of a
        {
            const std::unique_ptr<QMenu> menu = rig.data->contextMenu(rig.sourceRow(0));
            QMenu* filters = filtersOf(*menu);
            QVERIFY(filters != nullptr);
            QStringList titles;
            for (QAction* act : filters->actions()) {
                titles << act->text();
                QVERIFY(act->isCheckable());
            }
            QCOMPARE(titles, (QStringList{QStringLiteral("All"), QStringLiteral("Selected"),
                                          QStringLiteral("Fastest"), QStringLiteral("Fastest 3"),
                                          QStringLiteral("Within 1 % of Fastest"),
                                          QStringLiteral("Lap Numbers...")}));
            QVERIFY(actionOf(*filters, QStringLiteral("All"))->isChecked());
            QVERIFY(!rig.sourceRow(0).data(Qt::DecorationRole).isValid());
        }

        pick(rig, rig.sourceRow(0), QStringLiteral("Fastest"));
        QCOMPARE(rig.selection.filter(rig.a->id).kind, K::Fastest);
        QCOMPARE(rig.selection.filter(rig.b->id).kind, K::All); // per source
        QCOMPARE(rig.names(0), (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 2")}));
        // The Main out lap stays only because of its slot: icon and tool tip say so.
        const QModelIndex out = rig.range(0, QStringLiteral("Out Lap"));
        QVERIFY(out.data(Qt::DecorationRole).isValid());
        QVERIFY(out.data(Qt::ToolTipRole).toString().contains(QStringLiteral("selection slot")));
        QVERIFY(!rig.range(0, QStringLiteral("Lap 2")).data(Qt::DecorationRole).isValid());
        // The filtered source row shows the icon and names the filter.
        QVERIFY(rig.sourceRow(0).data(Qt::DecorationRole).isValid());
        QVERIFY(
            rig.sourceRow(0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("Filter: Fastest")));
        QVERIFY(!rig.sourceRow(1).data(Qt::DecorationRole).isValid());

        // From a range row of the same source; the menu checks the current kind.
        {
            const std::unique_ptr<QMenu> menu = rig.data->contextMenu(rig.range(0, QStringLiteral("Lap 2")));
            QVERIFY(actionOf(*filtersOf(*menu), QStringLiteral("Fastest"))->isChecked());
        }
        pick(rig, rig.range(0, QStringLiteral("Lap 2")), QStringLiteral("Fastest 3"));
        QCOMPARE(rig.names(0), (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 1"),
                                            QStringLiteral("Lap 2"), QStringLiteral("Lap 3")}));
        pick(rig, rig.sourceRow(0), QStringLiteral("Within 1 % of Fastest")); // 48 s; 49 s is 2 % slower
        QCOMPARE(rig.names(0), (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 2")}));
        rig.selection.toggleOverlay0(rangeRef(*rig.a, 3));
        pick(rig, rig.sourceRow(0), QStringLiteral("Selected"));
        QCOMPARE(rig.names(0), (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 3")}));
        pick(rig, rig.sourceRow(0), QStringLiteral("All"));
        QCOMPARE(rig.rows(0), 5);
        QVERIFY(!rig.sourceRow(0).data(Qt::DecorationRole).isValid());
        QVERIFY(!rig.range(0, QStringLiteral("Out Lap")).data(Qt::DecorationRole).isValid());
        // View state: written to project.json dataWindow, never to the undo stack.
        QCOMPARE(rig.session.undoStack().count(), 0);
        pick(rig, rig.sourceRow(1), QStringLiteral("Fastest 3"));
        QVERIFY(QString::fromStdString(rig.session.projectFile().dataWindow.compact())
                    .contains(QStringLiteral("fastest3")));
        QCOMPARE(rig.session.undoStack().count(), 0);
    }

    void lapNumbersEditor() {
        Rig rig("de-dwf-laps");
        rig.data->show();
        QVERIFY(QTest::qWaitForWindowExposed(rig.data.get()));
        pick(rig, rig.sourceRow(0), QStringLiteral("Lap Numbers..."));
        LapFilterEditor* editor = rig.data->lapFilterEditor();
        QVERIFY(editor != nullptr);
        QVERIFY(editor->isVisible());
        QVERIFY(!editor->isModal());
        QCOMPARE(editor->source(), rig.a->id);
        QCOMPARE(rig.selection.filter(rig.a->id).kind, K::All); // nothing applied yet
        editor->text().setText(QStringLiteral("1, 3"));
        QVERIFY(editor->error().text().isEmpty());
        QVERIFY(editor->apply());
        QCOMPARE(rig.selection.filter(rig.a->id).kind, K::LapNumbers);
        QCOMPARE(rig.selection.filter(rig.a->id).laps, std::string("1, 3"));
        QCOMPARE(rig.names(0),
                 (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 1"), QStringLiteral("Lap 3")}));
        QTRY_VERIFY(rig.data->lapFilterEditor() == nullptr); // closed (deleted on close)
        QVERIFY(rig.sourceRow(0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("(1, 3)")));

        // Reopened prefilled; a parse error shows while typing and Apply keeps the previous filter.
        editor = rig.data->editLapNumbers(rig.a->id);
        QCOMPARE(editor->text().text(), QStringLiteral("1, 3"));
        editor->text().setText(QStringLiteral("3,,5"));
        QVERIFY(editor->error().text().contains(QStringLiteral("expected a lap number")));
        QTest::keyClick(&editor->text(), Qt::Key_Return); // Apply
        QVERIFY(editor->isVisible());
        QVERIFY(!editor->error().text().isEmpty());
        QCOMPARE(rig.selection.filter(rig.a->id).laps, std::string("1, 3"));
        QVERIFY(!editor->apply());
        // A descending range is normalised: 5-3 = laps 3 to 5 (Lap 3 and the in lap, number 4).
        editor->text().setText(QStringLiteral(" 5-3 "));
        QVERIFY(editor->apply());
        QCOMPARE(rig.names(0),
                 (QStringList{QStringLiteral("Out Lap"), QStringLiteral("Lap 3"), QStringLiteral("In Lap")}));
        // Another source's editor replaces the open one.
        LapFilterEditor* first = rig.data->editLapNumbers(rig.a->id);
        LapFilterEditor* second = rig.data->editLapNumbers(rig.b->id);
        QCOMPARE(second->source(), rig.b->id);
        QTRY_VERIFY(first != second && rig.data->lapFilterEditor() == second);
        second->close();
    }

    void applyToAllSources() {
        Rig rig("de-dwf-all");
        pick(rig, rig.sourceRow(0), QStringLiteral("Fastest 3"));
        {
            const std::unique_ptr<QMenu> menu = rig.data->contextMenu(rig.sourceRow(0));
            QAction* all = actionOf(*menu, QStringLiteral("Apply Filter To All Sources"));
            QVERIFY(all != nullptr); // a local action without CommandActions
            all->trigger();
        }
        QVERIFY(rig.selection.applyToAllSources());
        QCOMPARE(rig.selection.filter(rig.b->id).kind, K::Fastest3);
        QCOMPARE(rig.rows(1), 3);
        // A later per-source change clears the flag (06 section 5.4).
        pick(rig, rig.sourceRow(1), QStringLiteral("All"));
        QVERIFY(!rig.selection.applyToAllSources());

        // With the shared actions the menu carries data.filter_all_sources itself.
        qt::KeymapBinder binder(rig.commands);
        CommandActions actions(rig.commands, binder);
        qt::registerSelectionCommands(rig.commands, rig.selection);
        rig.data->setActions(&actions);
        const std::unique_ptr<QMenu> menu = rig.data->contextMenu(rig.sourceRow(0));
        QAction* shared = actions.action(QStringLiteral("data.filter_all_sources"));
        QVERIFY(shared != nullptr);
        QVERIFY(menu->actions().contains(shared));
        QVERIFY(menu->actions().contains(actions.action(QStringLiteral("data.select_main"))));
        shared->trigger();
        QVERIFY(rig.selection.applyToAllSources());
        QCOMPARE(rig.selection.filter(rig.b->id).kind, K::Fastest3);
    }
};

QTEST_MAIN(TstDataWindowFilters)
#include "tst_data_window_filters.moc"
