// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesWindow (06 section 8; AC-M1-01 "a cursor drag updates the ValuesWindow rows for Main and Overlay 1"),
// offscreen: rows follow the focused component's channels (and stay when focus leaves every component), a
// cursor move updates the Main and Overlay 1 cells, the values follow the active worksheet (time vs distance
// alignment of the overlay), the "All channels" toggle and its signal, the pending styling and Copy Row.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/values_window.hpp>

#include <QCheckBox>
#include <QImage>
#include <QSignalSpy>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTest>
#include <cmath>
#include <memory>
#include <optional>

// The qtmodel synthetic-source helpers (in memory, circuit laps), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using esses::qt::test::makeSource;
using esses::qt::test::rangeRef;
using esses::qt::test::shippedUnits;
using esses::qt::test::SyntheticOptions;

namespace {

constexpr Micros kSec = 1'000'000;

class FakeComponent final : public ComponentWidget {
public:
    FakeComponent(WorksheetContext& context, QStringList channels)
        : ComponentWidget(project::ComponentSpec{}, context), channels_(std::move(channels)) {}
    const char* type() const override { return "fake"; }
    QStringList channelNames() const override { return channels_; }
    QStringList channels_;
};

project::WorkbookFile shippedWorkbook() {
    std::vector<std::string> log;
    auto wb = project::WorkbookFile::load(
        qt::test::repoRoot() / "data" / "workbooks" / "circuit-default.workbook.json", log);
    if (!wb) {
        qFatal("shipped workbook: %s", wb.error().toString().c_str());
    }
    return std::move(wb).value();
}

struct World {
    qt::AppRuntime rt{[] {
        qt::AppRuntimeOptions o;
        o.workers = 4;
        o.cacheBudget = std::size_t{256} << 20;
        o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
        return o;
    }()};
    qt::MathsService maths{rt.pool(), rt.dispatcher(), shippedUnits()};
    qt::DistanceService distance{maths, rt.dispatcher()};
    qt::DataSelectionModel selection;
    qt::WorkspaceViewModel workspace;
    qt::RenderService render{rt.pool(), rt.dispatcher(), selection, maths, distance};
    qt::CommandRegistry commands;
    project::WorkbookFile workbook = shippedWorkbook();
    std::unique_ptr<qt::WorksheetViewModel> time; // "Overlay", time domain
    std::unique_ptr<qt::WorksheetViewModel> dist; // "Driver", distance domain
    std::unique_ptr<WorksheetContext> context;
    qt::ValuesModel model{selection, maths};
    std::shared_ptr<const model::DataSource> a;

    World() {
        SyntheticOptions oa;
        oa.path = "values-a.ld";
        // Lap 2 (48 s) is Main, lap 3 (60 s) Overlay 1: at 12 s into the laps the throttle phases differ in
        // time mode (0.25 vs 0.20 of a lap) and agree at the same lap distance.
        oa.lapSeconds = {40.0, 50.0, 48.0, 60.0, 30.0};
        a = makeSource(oa, rt.cache());
        maths.addSource(a);
        distance.prefetch(a->id);
        selection.sourcePublished(a);
        time = std::make_unique<qt::WorksheetViewModel>(workbook.worksheets.at(0), workspace, selection,
                                                        distance);
        dist = std::make_unique<qt::WorksheetViewModel>(workbook.worksheets.at(1), workspace, selection,
                                                        distance);
        context.reset(new WorksheetContext{*time, selection, maths, render, commands, nullptr,
                                           QStringLiteral("values-test")});
    }
    bool ready() {
        for (std::size_t i = 1; i < 4; ++i) {
            const auto c = distance.chain(rangeRef(*a, i), Priority::Interactive);
            if (c == nullptr || !c->complete()) {
                return false;
            }
        }
        return true;
    }
    QString cell(int row, int column) const { return model.cellText(row, column); }
    bool settled(int row) const {
        for (const int c : {int(qt::ValuesModel::MainCol), int(qt::ValuesModel::ReferenceCol)}) {
            const QString t = cell(row, c);
            if (t.isEmpty() || t == QStringLiteral("…")) {
                return false;
            }
        }
        return true;
    }
};

// The box of the pixels in `cell` that differ from its first pixel (the cell's background), if any.
std::optional<QRect> inkBox(const QImage& image, const QRect& cell) {
    const QRect inside = cell.adjusted(1, 1, -1, -1) & image.rect();
    const QRgb background = image.pixel(inside.topLeft());
    std::optional<QRect> box;
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            if (image.pixel(x, y) != background) {
                box = box ? box->united(QRect(x, y, 1, 1)) : QRect(x, y, 1, 1);
            }
        }
    }
    return box;
}

} // namespace

class TstValuesWindow : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void followsTheFocusedComponent() {
        World w;
        ValuesWindow win(w.model);
        FakeComponent graph(*w.context, {QStringLiteral("Corr Speed"), QStringLiteral("Throttle")});
        FakeComponent other(*w.context, {QStringLiteral("Wheel Speed FL")});
        win.followComponent(&graph);
        QCOMPARE(win.model().rowCount(), 2);
        QCOMPARE(w.cell(1, qt::ValuesModel::NameCol), QStringLiteral("Throttle"));
        QVERIFY(win.followedComponent() == &graph);
        win.followComponent(nullptr); // focus left every component: the last list stays
        QCOMPARE(win.model().rowCount(), 2);
        win.followComponent(&other);
        QCOMPARE(win.model().rowCount(), 1);
        QCOMPARE(w.cell(0, qt::ValuesModel::NameCol), QStringLiteral("Wheel Speed FL"));
        // A channel added to the followed component appears after refreshChannels().
        other.channels_ << QStringLiteral("Throttle");
        win.refreshChannels();
        QCOMPARE(win.model().rowCount(), 2);
        QCOMPARE(win.table().model()->rowCount(), 2);
    }

    void cursorUpdatesMainAndOverlay1() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(), 30000);
        w.selection.setMain(rangeRef(*w.a, 2));
        w.selection.toggleOverlay(rangeRef(*w.a, 3)); // Overlay 1, the reference overlay
        ValuesWindow win(w.model);
        FakeComponent graph(*w.context, {QStringLiteral("Corr Speed"), QStringLiteral("Throttle")});
        win.followComponent(&graph);
        QCOMPARE(w.cell(1, qt::ValuesModel::MainCol), QString()); // no worksheet yet
        win.setWorksheet(w.time.get());
        QVERIFY(win.worksheet() == w.time.get());
        w.time->setCursor(0, qt::CursorSource::Click);
        QTRY_VERIFY(w.settled(1));
        // Lap start: throttle 100 % on both laps.
        QVERIFY(std::abs(w.cell(1, qt::ValuesModel::MainCol).toDouble() - 100.0) < 0.5);
        QVERIFY(std::abs(w.cell(1, qt::ValuesModel::ReferenceCol).toDouble() - 100.0) < 0.5);
        const QString mainBefore = w.cell(1, qt::ValuesModel::MainCol);
        const QString refBefore = w.cell(1, qt::ValuesModel::ReferenceCol);

        // A cursor drag (Click source) to 12 s: both rows change, in the window's table too.
        QSignalSpy changed(win.table().model(), &QAbstractItemModel::dataChanged);
        w.time->setCursor(12 * kSec, qt::CursorSource::Click);
        QVERIFY(changed.count() >= 1);
        QTRY_VERIFY(w.settled(1));
        QVERIFY(w.cell(1, qt::ValuesModel::MainCol) != mainBefore);
        QVERIFY(w.cell(1, qt::ValuesModel::ReferenceCol) != refBefore);
        // Time mode: Main at 0.25 of its lap (50 %), Overlay 1 at 0.20 of its lap (65.5 %).
        QVERIFY(std::abs(w.cell(1, qt::ValuesModel::MainCol).toDouble() - 50.0) < 1.0);
        QVERIFY(std::abs(w.cell(1, qt::ValuesModel::ReferenceCol).toDouble() - 65.45) < 1.0);
        QCOMPARE(win.table().model()->index(1, qt::ValuesModel::MainCol).data().toString(),
                 w.cell(1, qt::ValuesModel::MainCol));
        // Copy Row: name, Main value, unit.
        QCOMPARE(win.rowText(0), QStringLiteral("Corr Speed\t") + w.cell(0, qt::ValuesModel::MainCol) +
                                     QLatin1Char('\t') + w.cell(0, qt::ValuesModel::UnitCol));
    }

    void followsTheActiveWorksheet() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(), 30000);
        w.selection.setMain(rangeRef(*w.a, 2));
        w.selection.toggleOverlay(rangeRef(*w.a, 3));
        ValuesWindow win(w.model);
        FakeComponent graph(*w.context, {QStringLiteral("Throttle")});
        win.followComponent(&graph);
        win.setWorksheet(w.time.get());
        w.time->setCursor(12 * kSec, qt::CursorSource::Click);
        QTRY_VERIFY(w.settled(0));
        QVERIFY(std::abs(w.cell(0, qt::ValuesModel::MainCol).toDouble() -
                         w.cell(0, qt::ValuesModel::ReferenceCol).toDouble()) > 10.0);
        // The distance worksheet becomes active: Overlay 1 is read at the same lap distance, where the
        // throttle phase equals Main's.
        w.dist->activate();
        win.setWorksheet(w.dist.get());
        QVERIFY(win.worksheet() == w.dist.get());
        QTRY_VERIFY(w.settled(0) && std::abs(w.cell(0, qt::ValuesModel::MainCol).toDouble() -
                                             w.cell(0, qt::ValuesModel::ReferenceCol).toDouble()) < 1.0);
        // Re-pointing the model (every worksheet switch) leaves no connection behind on the worksheet it
        // left.
        for (int i = 0; i < 50; ++i) {
            win.setWorksheet(w.time.get());
            win.setWorksheet(w.dist.get());
        }
        QVERIFY(!QObject::disconnect(w.time.get(), nullptr, &w.model, nullptr));
        // No worksheet: empty cells.
        win.setWorksheet(nullptr);
        QVERIFY(!QObject::disconnect(w.dist.get(), nullptr, &w.model, nullptr));
        QCOMPARE(w.cell(0, qt::ValuesModel::MainCol), QString());
    }

    // The window's own delegate (one drawText a cell, D-217 (k)) puts each text where the standard item puts
    // it: the name left, the numbers right, the same rows, one line.
    void cellsDrawWhereTheStandardItemDoes() {
        World w;
        QTRY_VERIFY_WITH_TIMEOUT(w.ready(), 30000);
        w.selection.setMain(rangeRef(*w.a, 2));
        w.selection.toggleOverlay(rangeRef(*w.a, 3));
        ValuesWindow win(w.model);
        FakeComponent graph(*w.context, {QStringLiteral("Corr Speed"), QStringLiteral("Throttle")});
        win.followComponent(&graph);
        win.setWorksheet(w.time.get());
        w.time->setCursor(12 * kSec, qt::CursorSource::Click);
        QTRY_VERIFY(w.settled(0) && w.settled(1));
        win.resize(600, 300);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTableView& table = win.table();
        const QImage ours = table.viewport()->grab().toImage();
        QStyledItemDelegate standard;
        QAbstractItemDelegate* own = table.itemDelegate();
        QVERIFY(own != nullptr && dynamic_cast<QStyledItemDelegate*>(own) != nullptr);
        table.setItemDelegate(&standard);
        const QImage theirs = table.viewport()->grab().toImage();
        table.setItemDelegate(own);
        int compared = 0;
        for (int row = 0; row < table.model()->rowCount(); ++row) {
            for (int column = 0; column < table.model()->columnCount(); ++column) {
                const QRect cell = table.visualRect(table.model()->index(row, column));
                const std::optional<QRect> a = inkBox(ours, cell);
                const std::optional<QRect> b = inkBox(theirs, cell);
                QCOMPARE(a.has_value(), b.has_value());
                if (!a) {
                    continue;
                }
                const QString where =
                    QStringLiteral("row %1 column %2: ours (%3,%4)-(%5,%6), the standard's (%7,%8)-(%9,%10)")
                        .arg(row)
                        .arg(column)
                        .arg(a->left())
                        .arg(a->top())
                        .arg(a->right())
                        .arg(a->bottom())
                        .arg(b->left())
                        .arg(b->top())
                        .arg(b->right())
                        .arg(b->bottom());
                QVERIFY2(std::abs(a->left() - b->left()) <= 1 && std::abs(a->right() - b->right()) <= 1,
                         qPrintable(where));
                QVERIFY2(std::abs(a->top() - b->top()) <= 1 && std::abs(a->bottom() - b->bottom()) <= 1,
                         qPrintable(where));
                ++compared;
            }
        }
        QVERIFY(compared >= 8); // two names, two Main and two Overlay values, two units at least
    }

    void allChannelsToggle() {
        World w;
        ValuesWindow win(w.model);
        FakeComponent graph(*w.context, {QStringLiteral("Throttle")});
        win.followComponent(&graph);
        QSignalSpy toggled(&win, &ValuesWindow::allChannelsChanged);
        QVERIFY(!win.allChannels());
        win.allChannelsToggle().click();
        QVERIFY(win.allChannels());
        QCOMPARE(toggled.count(), 1);
        QCOMPARE(toggled.last().at(0).toBool(), true);
        // Every channel of the Main source: logged and built-in maths.
        QCOMPARE(win.model().rowCount(), 5);
        QCOMPARE(w.cell(0, qt::ValuesModel::NameCol), QStringLiteral("Corr Dist"));
        win.setAllChannels(false); // the shell restoring layout.json docks.values.allChannels
        QVERIFY(!win.allChannelsToggle().isChecked());
        QCOMPARE(win.model().rowCount(), 1);
    }
};

QTEST_MAIN(TstValuesWindow)
#include "tst_values_window.moc"
