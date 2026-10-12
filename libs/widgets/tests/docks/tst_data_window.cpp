// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataWindow (06 sections 5.2-5.3 and 6; AC-M1-01's ENTER / CTRL+ENTER / ALT+ENTER steps), offscreen, with
// the real key path: the shipped keymap in a KeymapBinder and the KeyEventRouter installed on the
// application, focus in the Data dock's tree. ENTER / keypad ENTER = setMain, CTRL+ENTER = toggleOverlay0,
// ALT+ENTER = toggleOverlay (compact renumbering) on the highlighted range; DELETE closes a source row only;
// UP/DOWN stay with the tree; CTRL+F (global) works from the dock and the highlight follows Main; clicks in
// the M / O / 1 columns; the Trusted checkbox; the slot colours and tool tips of the styling proxy.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/project/keymap.hpp>
#include <esses/project/scheme.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/key_event_router.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QFont>
#include <QIdentityProxyModel>
#include <QMainWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTreeView>
#include <memory>
#include <new>

// The qtmodel synthetic-source helpers (in memory, circuit laps), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::makeSource;
using esses::qt::test::rangeCount;
using esses::qt::test::rangeRef;
using esses::qt::test::SyntheticOptions;

namespace {

struct Rig {
    qt::AppRuntime rt{[] {
        qt::AppRuntimeOptions o;
        o.workers = 2;
        o.cacheBudget = std::size_t{128} << 20;
        o.markGuiThreadNoBlock = true; // as esses (AC-M1-12): the keys below run with the check armed
        return o;
    }()};
    qt::DataSelectionModel selection;
    qt::CommandRegistry commands;
    qt::KeymapBinder binder{commands};
    QMainWindow host;
    WorksheetCanvas* canvas = nullptr;
    DockHost* docks = nullptr;
    DataWindow* data = nullptr;
    KeyEventRouter* router = nullptr;
    std::shared_ptr<const model::DataSource> a;
    std::shared_ptr<const model::DataSource> b;

    Rig() {
        SyntheticOptions oa;
        oa.path = "dw-a.ld";
        oa.lapSeconds = {40.0, 50.0, 48.0, 49.0, 30.0};
        a = makeSource(oa, rt.cache());
        SyntheticOptions ob;
        ob.path = "dw-b.ld";
        ob.lapSeconds = {30.0, 45.0, 44.0, 20.0};
        b = makeSource(ob, rt.cache());
        selection.setSidecarCommit(
            [this](std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc) {
                selection.sourceReplaced(current->withSidecar(
                    std::move(doc), nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction()));
            });

        canvas = new WorksheetCanvas;
        docks = new DockHost(host, canvas);
        data = new DataWindow(selection, commands, &host);
        docks->addDock(QString::fromLatin1(DataWindow::kDockId), QStringLiteral("Data"), data, DockArea::Left,
                       DockState::Docked);
        qt::registerSelectionCommands(commands, selection);
        std::vector<std::string> log;
        auto keymap = project::Keymap::load(qt::test::repoRoot() / "data" / "keymaps" / "default.json", log);
        if (!keymap) {
            qFatal("default keymap: %s", keymap.error().toString().c_str());
        }
        binder.setKeymap(*keymap);
        router = new KeyEventRouter(binder, *docks, *canvas, &host);
        router->setScope(&host);
        qApp->installEventFilter(router);

        selection.sourcePublished(a);
        selection.sourcePublished(b);
        host.resize(900, 600);
    }
    ~Rig() { qApp->removeEventFilter(router); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    bool show() {
        host.show();
        if (!QTest::qWaitForWindowExposed(&host)) {
            return false;
        }
        host.activateWindow();
        if (!QTest::qWaitForWindowActive(&host)) {
            return false;
        }
        docks->focus(QString::fromLatin1(DataWindow::kDockId));
        return QApplication::focusWidget() == &data->tree();
    }
    QTreeView& tree() const { return data->tree(); }
    QModelIndex row(const model::RangeRef& r, int column = qt::DataWindowModel::NameCol) const {
        const QModelIndex i = data->viewModel().mapFromSource(data->model().indexOf(r));
        return i.siblingAtColumn(column);
    }
    QModelIndex sourceRow(int i) const { return data->viewModel().index(i, qt::DataWindowModel::NameCol); }
    void key(int k, Qt::KeyboardModifiers mods = Qt::NoModifier) const {
        QTest::keyClick(&tree(), static_cast<Qt::Key>(k), mods);
    }
};

} // namespace

class TstDataWindow : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void rowsAndSlotColumns() {
        Rig r;
        const auto& m = r.data->viewModel();
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.rowCount(m.index(0, 0)), static_cast<int>(rangeCount(*r.a)));
        QCOMPARE(m.rowCount(m.index(1, 0)), static_cast<int>(rangeCount(*r.b)));
        QCOMPARE(m.headerData(qt::DataWindowModel::MainCol, Qt::Horizontal).toString(), QStringLiteral("M"));
        // Main is the first range of the first source (06 section 5.2 sourcePublished).
        const model::RangeRef main = r.selection.snapshot().main;
        QVERIFY(main == rangeRef(*r.a, 0));
        QCOMPARE(r.row(main, qt::DataWindowModel::MainCol).data().toString(), QStringLiteral("●"));
        QVERIFY(r.row(main, qt::DataWindowModel::MainCol)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .startsWith(QStringLiteral("Main")));
        // The key colours come from the scheme (06 section 5.3).
        std::vector<std::string> log;
        auto scheme =
            project::Scheme::load(qt::test::repoRoot() / "data" / "schemes" / "light.scheme.json", log);
        QVERIFY(scheme.ok());
        const auto shared = std::make_shared<const project::Scheme>(std::move(scheme).value());
        r.data->setScheme(shared);
        const project::Rgba red = shared->role("cursorMain");
        const QColor c = r.row(main, qt::DataWindowModel::MainCol).data(Qt::ForegroundRole).value<QColor>();
        QCOMPARE(c, QColor(project::red(red), project::green(red), project::blue(red), project::alpha(red)));
        r.selection.toggleOverlay(rangeRef(*r.a, 2));
        const QModelIndex one = r.row(rangeRef(*r.a, 2), qt::DataWindowModel::OverlayCol);
        QCOMPARE(one.data().toString(), QStringLiteral("1"));
        const project::Rgba o1 = shared->overlayColour(1);
        QCOMPARE(one.data(Qt::ForegroundRole).value<QColor>(),
                 QColor(project::red(o1), project::green(o1), project::blue(o1), project::alpha(o1)));
        QCOMPARE(one.data(Qt::ToolTipRole).toString(), QStringLiteral("Overlay 1"));
        // Lap time and delta to the fastest lap of the source.
        QVERIFY(!r.row(rangeRef(*r.a, 2), qt::DataWindowModel::TimeCol).data().toString().isEmpty());
        QVERIFY(!r.row(rangeRef(*r.a, 1), qt::DataWindowModel::DeltaFastestCol).data().toString().isEmpty());
    }

    void enterSetsMain() {
        Rig r;
        QVERIFY(r.show());
        QSignalSpy invoked(&r.commands, &qt::CommandRegistry::invoked);
        r.data->highlight(rangeRef(*r.a, 3));
        r.key(Qt::Key_Return);
        QVERIFY(r.selection.snapshot().main == rangeRef(*r.a, 3));
        QCOMPARE(invoked.count(), 1);
        QCOMPARE(invoked.last().at(0).toString(), QStringLiteral("data.select_main"));
        r.data->highlight(rangeRef(*r.b, 2));
        r.key(Qt::Key_Enter, Qt::KeypadModifier); // keypad ENTER is bound too
        QVERIFY(r.selection.snapshot().main == rangeRef(*r.b, 2));
    }

    void ctrlEnterTogglesOverlay0() {
        Rig r;
        QVERIFY(r.show());
        r.data->highlight(rangeRef(*r.a, 2));
        r.key(Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(r.selection.snapshot().overlay0.has_value());
        QVERIFY(*r.selection.snapshot().overlay0 == rangeRef(*r.a, 2));
        QCOMPARE(r.row(rangeRef(*r.a, 2), qt::DataWindowModel::Overlay0Col).data().toString(),
                 QStringLiteral("■"));
        r.key(Qt::Key_Return, Qt::ControlModifier); // radio: off again
        QVERIFY(!r.selection.snapshot().overlay0.has_value());
        // On Main: no-op with a hint.
        QSignalSpy hint(&r.selection, &qt::DataSelectionModel::hint);
        r.data->highlight(r.selection.snapshot().main);
        r.key(Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(!r.selection.snapshot().overlay0.has_value());
        QCOMPARE(hint.count(), 1);
    }

    void altEnterTogglesOverlays() {
        Rig r;
        QVERIFY(r.show());
        r.data->highlight(rangeRef(*r.a, 2));
        r.key(Qt::Key_Return, Qt::AltModifier);
        r.data->highlight(rangeRef(*r.b, 1));
        r.key(Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(r.selection.overlayNumber(rangeRef(*r.a, 2)), 1);
        QCOMPARE(r.selection.overlayNumber(rangeRef(*r.b, 1)), 2);
        QCOMPARE(r.row(rangeRef(*r.b, 1), qt::DataWindowModel::OverlayCol).data().toString(),
                 QStringLiteral("2"));
        // Removing Overlay 1 renumbers compactly (D-017).
        r.data->highlight(rangeRef(*r.a, 2));
        r.key(Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(r.selection.overlayNumber(rangeRef(*r.a, 2)), -1);
        QCOMPARE(r.selection.overlayNumber(rangeRef(*r.b, 1)), 1);
        QCOMPARE(r.row(rangeRef(*r.b, 1), qt::DataWindowModel::OverlayCol).data().toString(),
                 QStringLiteral("1"));
    }

    // Key events are stack objects: a new event may reuse the address of one the tree accepted (so it never
    // propagated). The router takes a visit at the focus widget (or at the window when nothing has the focus)
    // as a new event, never as the propagation of the old one with the old focus chain.
    void aReusedEventAddressIsANewEvent() {
        Rig r;
        QVERIFY(r.show());
        r.data->highlight(rangeRef(*r.a, 2));
        const model::RangeRef mainBefore = r.selection.snapshot().main;
        alignas(QKeyEvent) unsigned char storage[sizeof(QKeyEvent)];
        auto* down = new (storage) QKeyEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(&r.tree(), down); // offered to the tree, which takes it (no propagation)
        QVERIFY(down->isAccepted());
        down->~QKeyEvent();
        QVERIFY(r.data->highlightedRange().has_value());
        QVERIFY(*r.data->highlightedRange() == rangeRef(*r.a, 3));
        // Nothing has the focus now; RETURN at the window, at the same address, is not bound for the window's
        // chain [Worksheet, Global]: it must not run data.select_main with the Data window's old chain.
        QApplication::focusWidget()->clearFocus();
        QTRY_VERIFY(QApplication::focusWidget() == nullptr);
        auto* enter = new (storage) QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&r.host, enter);
        enter->~QKeyEvent();
        QVERIFY(r.selection.snapshot().main == mainBefore);
    }

    void deleteClosesSourceRowsOnly() {
        Rig r;
        QVERIFY(r.show());
        r.data->highlight(rangeRef(*r.b, 1));
        r.key(Qt::Key_Delete); // a range row: no-op
        QCOMPARE(r.selection.sources().size(), std::size_t{2});
        r.tree().setCurrentIndex(r.sourceRow(1));
        QVERIFY(r.data->highlightedIsSource());
        r.key(Qt::Key_Delete);
        QCOMPARE(r.selection.sources().size(), std::size_t{1});
        QCOMPARE(r.data->viewModel().rowCount(), 1);
        // A custom close path (the shell's SourceLoader::close).
        model::SourceId closed;
        r.data->setCloseSource([&closed](const model::SourceId& id) { closed = id; });
        r.tree().setCurrentIndex(r.sourceRow(0));
        r.key(Qt::Key_Delete);
        QVERIFY(closed == r.a->id);
    }

    void arrowsStayInTheTree() {
        Rig r;
        QVERIFY(r.show());
        QSignalSpy invoked(&r.commands, &qt::CommandRegistry::invoked);
        const model::RangeRef main = r.selection.snapshot().main;
        r.data->highlight(rangeRef(*r.a, 1));
        r.key(Qt::Key_Down);
        QVERIFY(r.data->highlightedRange().has_value());
        QVERIFY(*r.data->highlightedRange() == rangeRef(*r.a, 2));
        r.key(Qt::Key_Up);
        QVERIFY(*r.data->highlightedRange() == rangeRef(*r.a, 1));
        // A plain letter is the tree's keyboard search, never a worksheet command (N is not bound here).
        r.key(Qt::Key_N);
        QVERIFY(r.selection.snapshot().main == main);
        QCOMPARE(invoked.count(), 0);
    }

    void highlightFollowsMain() {
        Rig r;
        QVERIFY(r.show());
        // CTRL+F is global: it works from inside the dock (06 section 3.3 item 1).
        r.key(Qt::Key_F, Qt::ControlModifier);
        const model::RangeRef fastest = rangeRef(*r.a, 2); // 48 s, the fastest flying lap of a
        QVERIFY(r.selection.snapshot().main == fastest);
        QVERIFY(r.data->highlightedRange().has_value());
        QVERIFY(*r.data->highlightedRange() == fastest);
        r.selection.nextRange(); // N
        QVERIFY(*r.data->highlightedRange() == rangeRef(*r.a, 3));
        r.selection.prevRange(); // P
        QVERIFY(*r.data->highlightedRange() == fastest);
    }

    void mouseColumns() {
        Rig r;
        QVERIFY(r.show());
        const auto click = [&r](const model::RangeRef& range, int column) {
            const QModelIndex i = r.row(range, column);
            QTest::mouseClick(r.tree().viewport(), Qt::LeftButton, {}, r.tree().visualRect(i).center());
        };
        click(rangeRef(*r.a, 3), qt::DataWindowModel::MainCol);
        QVERIFY(r.selection.snapshot().main == rangeRef(*r.a, 3));
        click(rangeRef(*r.a, 2), qt::DataWindowModel::Overlay0Col);
        QVERIFY(r.selection.snapshot().overlay0 && *r.selection.snapshot().overlay0 == rangeRef(*r.a, 2));
        click(rangeRef(*r.b, 2), qt::DataWindowModel::OverlayCol);
        QCOMPARE(r.selection.overlayNumber(rangeRef(*r.b, 2)), 1);
        // A double-click on the name sets Main (the view's signal; the cramped offscreen dock leaves the Name
        // cell mostly under the tree decoration).
        Q_EMIT r.tree().doubleClicked(r.row(rangeRef(*r.b, 1)));
        QVERIFY(r.selection.snapshot().main == rangeRef(*r.b, 1));
    }

    void trustCheckbox() {
        Rig r;
        const model::RangeRef lap = rangeRef(*r.a, 2);
        const QModelIndex trusted = r.row(lap, qt::DataWindowModel::TrustedCol);
        QCOMPARE(trusted.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QVERIFY(r.data->viewModel().flags(trusted).testFlag(Qt::ItemIsUserCheckable));
        QVERIFY(r.data->viewModel().setData(trusted, Qt::Unchecked, Qt::CheckStateRole));
        QTRY_COMPARE(r.row(lap, qt::DataWindowModel::TrustedCol).data(Qt::CheckStateRole).toInt(),
                     int(Qt::Unchecked));
        // Untrusted rows are italic (not only grey: 06 section 20 item 4).
        QVERIFY(r.row(lap).data(Qt::FontRole).value<QFont>().italic());
        QVERIFY(r.row(lap).data(Qt::ToolTipRole).toString().contains(QStringLiteral("ntrusted")));
    }
};

QTEST_MAIN(TstDataWindow)
#include "tst_data_window.moc"
