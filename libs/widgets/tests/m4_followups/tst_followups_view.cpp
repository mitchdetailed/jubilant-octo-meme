// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M4 followups of esses_widgets, the view (D-211 (e)-(f); docs/milestones/M3.md section 6 items 21 and 27),
// offscreen:
//   (e) the time_distance group headings of 04 section 8.2 item 4: a GroupHeading hit over each group's key
//       column outside its labels, dragging a heading moves the group (one undoable edit, traces, unknown
//       members and vertical zoom along, explicit panel heights kept; the zoom also follows undo and redo and
//       stays with a group edited in place, D-217), a double-click opens Group Properties;
//   (f) links (04 section 2.4): a component with links.cursor, links.zoom or links.datum off keeps its own
//       cursor, zoom or datum, starting from the worksheet's; the worksheet's changes of that part do not
//       reach it, its own mouse and the keyboard rows offered to it change only its own; the outing graph's
//       zoom stays linked; switching a link on again shows the worksheet's;
//   (g) the shell takes SourceLoader::summariesReady to every component of the window (M3 item 18).
// Expectations come from 04 sections 2.4, 8.2 and 8.6 and the synthetic source's geometry (laps of 40, 50,
// 48, 49 and 30 s at 50 Hz; component_test_support).

#include "components/plots_test_support.hpp"
#include "properties_shell/shell_test_support.hpp"

#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/values_list.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/shell/followups_shell.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

project::ComponentSpec unlinked(project::ComponentSpec spec, bool zoom, bool cursor, bool datum) {
    spec.links.zoom = zoom;
    spec.links.cursor = cursor;
    spec.links.datum = datum;
    return spec;
}

QJsonObject bodyOf(const ComponentWidget& c) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(c.spec().body.compact())).object();
}

QStringList groupNames(const TimeDistanceGraph& g) {
    QStringList out;
    for (const plot::TdGroup& group : g.body().groups) {
        out << QString::fromStdString(group.name);
    }
    return out;
}

// A point (widget-logical) inside group `group`'s key column below its labels: the group's heading.
std::optional<QPointF> headingPoint(const TimeDistanceGraph& g, int group) {
    const double dpr = g.layoutDevicePixelRatio() > 0.0 ? g.layoutDevicePixelRatio() : 1.0;
    for (const plot::TdGroupLayout& gl : g.layout().groups) {
        if (gl.group != group || gl.keyGeometry.rowRects.empty()) {
            continue;
        }
        const plot::RectPx last = gl.keyGeometry.rowRects.back();
        const float below = gl.key.y + last.y + last.h;
        if (gl.key.y + gl.key.h - below < 4.0F) {
            return std::nullopt;
        }
        return QPointF(static_cast<double>(gl.key.x + 4.0F) / dpr,
                       static_cast<double>(below + 0.5F * (gl.key.y + gl.key.h - below)) / dpr);
    }
    return std::nullopt;
}

} // namespace

class TstFollowupsView : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void cleanup() {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* d = qobject_cast<PropertiesDialog*>(top)) {
                delete d;
            }
        }
    }

    // (e) the heading regions, a drag that reorders, a double-click for Group Properties.
    void groupHeadingsDragAndProperties() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<TimeDistanceGraph> rig(
            w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}, {"Wheel Speed FR"}}), QSize(700, 480));
        TimeDistanceGraph& g = *rig.c;
        QVERIFY(waitQuiet(g));
        QCOMPARE(groupNames(g), (QStringList{QStringLiteral("Wheel Speed FL"), QStringLiteral("Throttle"),
                                             QStringLiteral("Wheel Speed FR")}));
        const std::optional<QPointF> h0 = headingPoint(g, 0);
        const std::optional<QPointF> h2 = headingPoint(g, 2);
        QVERIFY(h0 && h2);
        QVERIFY(g.hitAt(*h0).kind == plot::HitKind::GroupHeading);
        QCOMPARE(g.hitAt(*h0).group, 0);
        QCOMPARE(g.hitAt(*h2).group, 2);
        // The labels keep their own hits (a click there makes the channel active).
        const plot::TdGroupLayout& gl1 = g.layout().groups[1];
        const double dpr = g.layoutDevicePixelRatio();
        const plot::RectPx label = gl1.keyGeometry.rowRects.front();
        QVERIFY(g.hitAt(QPointF(static_cast<double>(gl1.key.x + label.x + 2.0F) / dpr,
                                static_cast<double>(gl1.key.y + label.y + 0.5F * label.h) / dpr))
                    .kind == plot::HitKind::KeyLabel);

        // Drag group 0's heading below the last group's middle: it becomes the last group.
        sendMouse(&g, QEvent::MouseButtonPress, *h0, Qt::LeftButton, Qt::LeftButton);
        QVERIFY(!g.groupDropIndex()); // nothing before the pointer moves
        sendMouse(&g, QEvent::MouseMove, QPointF(h0->x(), h2->y()), Qt::NoButton, Qt::LeftButton);
        QVERIFY(g.groupDropIndex() == std::optional<int>(2));
        const int edits = rig.edits;
        sendMouse(&g, QEvent::MouseButtonRelease, QPointF(h0->x(), h2->y()), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(rig.edits, edits + 1); // one undoable spec edit
        QCOMPARE(rig.lastUndoText, QStringLiteral("Move group Wheel Speed FL"));
        QVERIFY(!g.groupDropIndex());
        QCOMPARE(groupNames(g), (QStringList{QStringLiteral("Throttle"), QStringLiteral("Wheel Speed FR"),
                                             QStringLiteral("Wheel Speed FL")}));
        QCOMPARE(QString::fromStdString(g.body().groups[2].traces.front().channel),
                 QStringLiteral("Wheel Speed FL")); // the traces went along
        // A drag that ends where it started edits nothing.
        QVERIFY(waitQuiet(g));
        const std::optional<QPointF> h1 = headingPoint(g, 1);
        QVERIFY(h1);
        sendMouse(&g, QEvent::MouseButtonPress, *h1, Qt::LeftButton, Qt::LeftButton);
        sendMouse(&g, QEvent::MouseButtonRelease, *h1, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(rig.edits, edits + 1);

        // Double-click: Group Properties at that group's record.
        QVERIFY(PropertiesDialog::find(g) == nullptr);
        doubleClick(&g, *h1);
        PropertiesDialog* dialog = PropertiesDialog::find(g);
        QVERIFY(dialog != nullptr);
        auto* groups = dialog->findChild<RecordListEditor*>(QStringLiteral("/groups"));
        QVERIFY(groups != nullptr);
        QCOMPARE(groups->currentIndex(), 1);
        QVERIFY(dialog->tabs().currentWidget()->isAncestorOf(groups));
        QVERIFY(dialog->focusRecord(QStringLiteral("/groups"), 0));
        QCOMPARE(groups->currentIndex(), 0);
        QVERIFY(!dialog->focusRecord(QStringLiteral("/groups"), 7));
        QVERIFY(g.openGroupProperties(9) == nullptr);
    }

    // (e) panels with explicit edges keep their heights; unknown members and the vertical zoom go along.
    void movingAGroupKeepsItsHeightAndItsMembers() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        project::ComponentSpec spec =
            timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}, {"Wheel Speed FR"}});
        QJsonObject body = QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
        QJsonArray groups = body.value(QStringLiteral("groups")).toArray();
        const double edges[3][2] = {{0.0, 0.5}, {0.5, 0.8}, {0.8, 1.0}};
        for (int i = 0; i < 3; ++i) {
            QJsonObject gj = groups[i].toObject();
            gj.insert(QStringLiteral("edges"), QJsonArray{edges[i][0], edges[i][1]});
            groups[i] = gj;
        }
        QJsonObject first = groups[0].toObject();
        first.insert(QStringLiteral("x-i2"), QJsonObject{{QStringLiteral("keep"), 1}});
        groups[0] = first;
        body.insert(QStringLiteral("groups"), groups);
        spec.body = JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString()).value();
        Rig<TimeDistanceGraph> rig(w, spec, QSize(700, 480));
        TimeDistanceGraph& g = *rig.c;
        QVERIFY(waitQuiet(g));
        g.setVerticalZoom(0, plot::TdGroupZoom{0.25, 0.5});
        const project::ComponentSpec beforeMove = g.spec();
        QVERIFY(!g.moveGroup(0, 0));
        QVERIFY(!g.moveGroup(0, 3));
        QVERIFY(!g.moveGroup(-1, 1));
        QVERIFY(g.moveGroup(0, 2));
        // Heights 0.3, 0.2 and 0.5 stacked from 0 in the new order.
        const std::vector<plot::TdGroup>& moved = g.body().groups;
        QCOMPARE(QString::fromStdString(moved[2].name), QStringLiteral("Wheel Speed FL"));
        QCOMPARE(moved[0].edgeTop, 0.0);
        QVERIFY(std::abs(moved[0].edgeBottom - 0.3) < 1e-12);
        QVERIFY(std::abs(moved[1].edgeTop - 0.3) < 1e-12);
        QVERIFY(std::abs(moved[1].edgeBottom - 0.5) < 1e-12);
        QVERIFY(std::abs(moved[2].edgeTop - 0.5) < 1e-12);
        QCOMPARE(moved[2].edgeBottom, 1.0);
        const QJsonArray after = bodyOf(g).value(QStringLiteral("groups")).toArray();
        QCOMPARE(after[2]
                     .toObject()
                     .value(QStringLiteral("x-i2"))
                     .toObject()
                     .value(QStringLiteral("keep"))
                     .toInt(),
                 1);
        QVERIFY(!after[0].toObject().contains(QStringLiteral("x-i2")));
        QVERIFY(g.verticalZoomOf(2) == (plot::TdGroupZoom{0.25, 0.5}));
        QVERIFY(g.verticalZoomOf(0) == plot::TdGroupZoom{});
        // Regression (PR #5 review U2, D-217): undo and redo reach the graph as applySpec (the canvas's
        // syncInPlace); the vertical zoom follows its group back and forth.
        const project::ComponentSpec afterMove = g.spec();
        g.applySpec(beforeMove); // undo
        QCOMPARE(QString::fromStdString(g.body().groups[0].name), QStringLiteral("Wheel Speed FL"));
        QVERIFY(g.verticalZoomOf(0) == (plot::TdGroupZoom{0.25, 0.5}));
        QVERIFY(g.verticalZoomOf(2) == plot::TdGroupZoom{});
        g.applySpec(afterMove); // redo
        QVERIFY(g.verticalZoomOf(2) == (plot::TdGroupZoom{0.25, 0.5}));
        QVERIFY(g.verticalZoomOf(0) == plot::TdGroupZoom{});
    }

    // (e) A group edited in place keeps its vertical zoom (a channel added or removed, a rename); when a
    // group is added or removed, the others' zoom stays with its group (PR #5 re-verification of U2, D-217).
    void editingAGroupInPlaceKeepsItsZoom() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        Rig<TimeDistanceGraph> rig(w, timeDistanceSpec({{"Wheel Speed FL"}, {"Throttle"}}), QSize(700, 480));
        TimeDistanceGraph& g = *rig.c;
        QVERIFY(waitQuiet(g));
        const plot::TdGroupZoom speed{0.25, 0.5};
        const plot::TdGroupZoom throttle{0.125, 0.875};
        g.setVerticalZoom(0, speed);
        g.setVerticalZoom(1, throttle);
        QVERIFY(g.verticalZoomOf(0) == speed);
        QVERIFY(g.verticalZoomOf(1) == throttle);
        // An edit of the groups array, applied as the canvas's syncInPlace applies edits, undo and redo.
        const auto applyEdit = [&g](const auto& edit) {
            QJsonObject body = bodyOf(g);
            QJsonArray groups = body.value(QStringLiteral("groups")).toArray();
            edit(groups);
            body.insert(QStringLiteral("groups"), groups);
            project::ComponentSpec next = g.spec();
            next.body =
                JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString()).value();
            g.applySpec(next);
        };

        // Wheel Speed FR joins the speed group (like units) through the component's own edit.
        const int edits = rig.edits;
        g.addChannel(QStringLiteral("Wheel Speed FR"), false);
        QCOMPARE(rig.edits, edits + 1);
        QCOMPARE(rig.lastUndoText, QStringLiteral("Add Wheel Speed FR"));
        QCOMPARE(g.body().groups.size(), std::size_t{2});
        QCOMPARE(g.body().groups[0].traces.size(), std::size_t{2});
        QVERIFY(g.verticalZoomOf(0) == speed);
        QVERIFY(g.verticalZoomOf(1) == throttle);

        // The throttle group renamed (Group Properties), then a trace removed from the speed group.
        applyEdit([](QJsonArray& groups) {
            QJsonObject t = groups[1].toObject();
            t.insert(QStringLiteral("name"), QStringLiteral("Pedal"));
            groups[1] = t;
        });
        QCOMPARE(groupNames(g).value(1), QStringLiteral("Pedal"));
        QVERIFY(g.verticalZoomOf(0) == speed);
        QVERIFY(g.verticalZoomOf(1) == throttle);
        applyEdit([](QJsonArray& groups) {
            QJsonObject s = groups[0].toObject();
            QJsonArray traces = s.value(QStringLiteral("traces")).toArray();
            traces.removeAt(1);
            s.insert(QStringLiteral("traces"), traces);
            groups[0] = s;
        });
        QCOMPARE(g.body().groups[0].traces.size(), std::size_t{1});
        QVERIFY(g.verticalZoomOf(0) == speed);
        QVERIFY(g.verticalZoomOf(1) == throttle);

        // A new group at the bottom (CTRL): the others keep theirs, the new one has none.
        g.addChannel(QStringLiteral("Wheel Speed FR"), true);
        QCOMPARE(g.body().groups.size(), std::size_t{3});
        QVERIFY(g.verticalZoomOf(0) == speed);
        QVERIFY(g.verticalZoomOf(1) == throttle);
        QVERIFY(g.verticalZoomOf(2) == plot::TdGroupZoom{});

        // The speed group removed: the throttle group's zoom moves up with it, none lands on Wheel Speed FR.
        applyEdit([](QJsonArray& groups) { groups.removeAt(0); });
        QCOMPARE(groupNames(g), (QStringList{QStringLiteral("Pedal"), QStringLiteral("Wheel Speed FR")}));
        QVERIFY(g.verticalZoomOf(0) == throttle);
        QVERIFY(g.verticalZoomOf(1) == plot::TdGroupZoom{});
    }

    // (f) an unlinked cursor, zoom and datum.
    void unlinkedComponentsKeepTheirOwnView() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1)); // zoom [0, 50 s), cursor 0
        QVERIFY(QTest::qWaitFor([&] { return w.distanceReady(src); }, kTimeoutMs));
        Rig<TimeDistanceGraph> linked(w, timeDistanceSpec({{"Wheel Speed FL"}}), QSize(600, 300));
        Rig<TimeDistanceGraph> own(w, unlinked(timeDistanceSpec({{"Wheel Speed FL"}}), false, false, false),
                                   QSize(600, 300));
        QVERIFY(waitQuiet(*linked.c) && waitQuiet(*own.c));
        TimeDistanceGraph& a = *linked.c;
        TimeDistanceGraph& b = *own.c;
        QVERIFY(a.zoomLinked() && a.cursorLinked() && a.datumLinked());
        QVERIFY(!b.zoomLinked() && !b.cursorLinked() && !b.datumLinked());
        const plot::XSpan lap{0.0, 50.0 * kSec};
        QVERIFY(b.viewState().zoom == lap); // taken from the worksheet at creation
        QCOMPARE(b.viewState().cursorT, Micros{0});

        // The worksheet's cursor, zoom and datum move; the unlinked graph keeps its own.
        const int dynamicBefore = b.dynamicBuilds();
        w.view->setCursor(5 * kSec, qt::CursorSource::Programmatic);
        w.view->zoomTo(plot::XSpan{0.0, 10.0 * kSec});
        w.view->placeDatum();
        QCOMPARE(a.viewState().cursorT, 5 * kSec);
        QVERIFY(a.viewState().zoom == (plot::XSpan{0.0, 10.0 * kSec}));
        QVERIFY(a.viewState().datumT == std::optional<Micros>(5 * kSec));
        QCOMPARE(b.viewState().cursorT, Micros{0});
        QVERIFY(b.viewState().zoom == lap);
        QVERIFY(!b.viewState().datumT);
        QCOMPARE(b.dynamicBuilds(), dynamicBefore); // nothing reached its layers
        QVERIFY(waitQuiet(b));
        QVERIFY(b.inputs().view.zoom == lap); // its Static layer is built over its own zoom

        // Its own mouse: a click in the trace area places its own cursor, not the worksheet's.
        const plot::RectPx area = b.layout().traceArea;
        const double dpr = b.layoutDevicePixelRatio();
        const double xMid = static_cast<double>(area.x + 0.5F * area.w) / dpr;
        const double yMid = static_cast<double>(area.y + 0.5F * area.h) / dpr;
        click(&b, QPointF(xMid, yMid));
        QVERIFY(std::abs(static_cast<double>(b.viewState().cursorT) - 25.0 * kSec) < 0.2 * kSec);
        QCOMPARE(w.view->state().cursorT, 5 * kSec);
        // The wheel zooms its own zoom (x0.8 about the pointer).
        sendWheel(&b, QPointF(xMid, yMid), 120);
        QVERIFY(std::abs(b.viewState().zoom.length() - 0.8 * lap.length()) < 1.0);
        QVERIFY(w.view->state().zoom == (plot::XSpan{0.0, 10.0 * kSec}));

        // The keyboard rows the shell offers the focused component act on its own parts only.
        const Micros cursor = b.viewState().cursorT;
        QVERIFY(b.handleCommand(QStringLiteral("cursor.right"), {}));
        QVERIFY(b.viewState().cursorT > cursor);
        QVERIFY(b.handleCommand(QStringLiteral("cursor.home"), {}));
        QCOMPARE(static_cast<double>(b.viewState().cursorT), std::round(b.viewState().zoom.a));
        const plot::XSpan before = b.viewState().zoom;
        QVERIFY(b.handleCommand(QStringLiteral("zoom.in"), {})); // x0.5 about its cursor
        QVERIFY(std::abs(b.viewState().zoom.length() - 0.5 * before.length()) < 1.0);
        QVERIFY(b.handleCommand(QStringLiteral("zoom.default_range"), {})); // W: [0, |Main|)
        QVERIFY(b.viewState().zoom == lap);
        QVERIFY(b.handleCommand(QStringLiteral("pan.right"), {})); // +10 % of the span
        QVERIFY(std::abs(b.viewState().zoom.a - 0.1 * lap.length()) < 1.0);
        b.viewZoomFull(); // F2's rule: the whole Main outing, [o.start - M.start, o.end - M.start)
        const double lapStart = static_cast<double>(lapAt(*src, 1).start);
        QCOMPARE(b.viewState().zoom.a, -lapStart);
        QCOMPARE(b.viewState().zoom.b, 217.0 * kSec - lapStart);
        QVERIFY(b.handleCommand(QStringLiteral("datum.place"), {}));
        QVERIFY(b.viewState().datumT == std::optional<Micros>(b.viewState().cursorT));
        QVERIFY(b.handleCommand(QStringLiteral("datum.toggle"), {}));
        QVERIFY(!b.viewState().datumT);
        QVERIFY(b.handleCommand(QStringLiteral("datum.toggle"), {}));
        QVERIFY(b.viewState().datumT.has_value());
        // The worksheet saw none of it; the linked graph's rows go to the worksheet.
        QCOMPARE(w.view->state().cursorT, 5 * kSec);
        QVERIFY(w.view->state().zoom == (plot::XSpan{0.0, 10.0 * kSec}));
        QVERIFY(!a.handleCommand(QStringLiteral("cursor.right"), {}));
        QVERIFY(!a.handleCommand(QStringLiteral("zoom.in"), {}));

        // F9: the own zoom restarts from the worksheet's zoom in the new domain.
        w.view->setDomain(plot::Domain::Distance);
        QVERIFY(b.viewState().domain == plot::Domain::Distance);
        QVERIFY(b.viewState().zoom == w.view->state().zoom);
        w.view->setDomain(plot::Domain::Time);

        // Links switched on again: the worksheet's cursor, zoom and datum show (delivered once).
        b.applySpec(unlinked(b.spec(), true, true, true));
        QCOMPARE(b.viewState().cursorT, w.view->state().cursorT);
        QVERIFY(b.viewState().zoom == w.view->state().zoom);
        QVERIFY(b.viewState().datumT == w.view->state().datumT);
        QVERIFY(waitQuiet(b));
        QVERIFY(b.inputs().view.zoom == w.view->state().zoom);
        // And off again: they start from the worksheet's values.
        b.applySpec(unlinked(b.spec(), true, false, true));
        QCOMPARE(b.viewState().cursorT, w.view->state().cursorT);
        w.view->setCursor(7 * kSec, qt::CursorSource::Programmatic);
        QCOMPARE(b.viewState().cursorT, 5 * kSec);
        QVERIFY(b.viewState().zoom == w.view->state().zoom); // the zoom is linked
    }

    // (f) the outing graph is the zoom control; a values list reads its values at its own cursor.
    void outingZoomStaysLinkedAndValuesFollowTheOwnCursor() {
        World w;
        const auto src = w.publish(SourceOptions{});
        selectMain(w, lapRef(*src, 1));
        const project::ComponentSpec outing = unlinked(
            specOf("outing", R"({"groups":[{"name":"Speed","traces":[{"channel":"Wheel Speed FL"}]}]})"),
            false, true, true);
        Rig<OutingGraph> o(w, outing, QSize(600, 200));
        QVERIFY(o.c->zoomLinked()); // links.zoom off is ignored (04 section 9 item 2)
        w.view->zoomTo(plot::XSpan{0.0, 10.0 * kSec});
        QVERIFY(o.c->viewState().zoom == w.view->state().zoom);

        project::ComponentSpec list = unlinked(valuesListSpec({"Throttle"}), true, false, true);
        std::unique_ptr<ComponentWidget> owner(ComponentWidget::create(list, *w.context));
        auto* values = qobject_cast<ValuesList*>(owner.get());
        QVERIFY(values != nullptr);
        const SourceShape shape;
        const auto cellAt = [&](double seconds) {
            return QString::number(
                shape.throttle(static_cast<std::size_t>(std::llround((40.0 + seconds) * 50.0))), 'f', 2);
        };
        QTRY_COMPARE_WITH_TIMEOUT(values->model().cellText(0, qt::ValuesModel::MainCol), cellAt(0.0),
                                  kTimeoutMs);
        w.view->setCursor(10 * kSec, qt::CursorSource::Programmatic); // the worksheet's cursor moves
        QCOMPARE(values->model().cellText(0, qt::ValuesModel::MainCol), cellAt(0.0)); // its own did not
        values->setViewCursor(20 * kSec, qt::CursorSource::Click);
        QTRY_COMPARE_WITH_TIMEOUT(values->model().cellText(0, qt::ValuesModel::MainCol), cellAt(20.0),
                                  kTimeoutMs);
    }

    // (g) M3 item 18: the summary pass's end reaches every component, shown or kept hidden.
    void summariesReadyReachesEveryComponent() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        std::unique_ptr<qt::AppRuntime> rt = shelltest::runtime(2);
        auto w = std::make_unique<MainWindow>(*rt, shelltest::pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        QVERIFY(shelltest::showAndActivate(*w));
        auto* followups = shellExtension<FollowupsShell>(*w);
        QVERIFY(followups != nullptr);
        const auto components = w->canvas().findChildren<ComponentWidget*>();
        QVERIFY(!components.isEmpty()); // the shipped workbook's worksheet
        Q_EMIT w->loader().summariesReady(model::SourceId{});
        QCOMPARE(followups->lastSummaryDeliveries(), static_cast<int>(components.size()));
        w.reset();
        rt->shutdown();
    }
};

QTEST_MAIN(TstFollowupsView)
#include "tst_followups_view.moc"
