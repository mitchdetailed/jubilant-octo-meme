// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapWindow (06 section 9; 04 section 14 item 7), offscreen: the embedded TrackPosition over the active
// worksheet's context with the links forced on, rebuilt on a context switch with the dock's own settings
// kept, the "No worksheet" note, the context-menu actions (Fit, Show Sections, Show Section Labels, Track
// Editor..., Regenerate Track...) and the dock's edits applying at once without an undo command.

#include "components/reports_track/rt_support.hpp"

#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>

#include <QAction>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::rtw;

class TstTrackMapWindow : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void embedsATrackPositionThatFollowsTheWorksheet() {
        World w;
        qt::TrackMapService maps(nullptr);
        w.render.setTrackMaps(&maps);
        QVERIFY(maps.setMap("Rect Venue", std::make_shared<const analysis::TrackMap>(rectMap("Rect Venue")))
                    .ok());
        const auto src = publish(
            w, linearSource("tmw", "Rect Venue", {40.0, 50.0, 30.0}, 2000.0, *shippedUnits(), w.rt.cache()));
        w.selection.setMain(lapRef(*src, 1));

        TrackMapWindow dock;
        dock.resize(320, 240);
        dock.show();
        QVERIFY(dock.trackPosition() == nullptr);
        auto* note = dock.findChild<QLabel*>();
        QVERIFY(note != nullptr && note->isVisible());
        dock.setContext(w.context.get());
        TrackPosition* tp = dock.trackPosition();
        QVERIFY(tp != nullptr);
        QCOMPARE(tp->parentWidget(), &dock);
        QVERIFY(note->isHidden());
        QVERIFY(tp->spec().links.zoom && tp->spec().links.cursor && tp->spec().links.datum);
        QVERIFY(!tp->spec().caption.show); // the dock's title names it
        QVERIFY(QTest::qWaitFor([&] { return tp->staticUpToDate(); }, 20000));
        QVERIFY(tp->trackMap() != nullptr);

        // Context menu actions change the dock's settings at once (no undo command: not a document).
        QSignalSpy editor(&dock, &TrackMapWindow::editorRequested);
        QSignalSpy regenerate(&dock, &TrackMapWindow::regenerateRequested);
        QVERIFY(dock.actions().contains(dock.fitAction()));
        QVERIFY(dock.showSectionsAction()->isChecked());
        dock.showSectionsAction()->trigger();
        QVERIFY(!tp->body().showSectionColours && !tp->body().showSectionMarks);
        QVERIFY(!dock.showSectionLabelsAction()->isChecked());
        dock.showSectionLabelsAction()->trigger();
        QVERIFY(tp->body().showSectionLabels);
        dock.editorAction()->trigger();
        dock.regenerateAction()->trigger();
        QCOMPARE(editor.count(), 1);
        QCOMPARE(regenerate.count(), 1);
        // PLUS in the dock zooms (the embedded widget takes the key itself); the edit lands in the dock spec.
        tp->setFocus();
        QTest::keyClick(tp, Qt::Key_Plus);
        QCOMPARE(tp->body().zoom, TrackPosition::kZoomStep);
        tp->commitView();
        QCOMPARE(objectOf(dock.spec().body)
                     .value(QStringLiteral("scale"))
                     .toObject()
                     .value(QStringLiteral("zoom"))
                     .toDouble(),
                 TrackPosition::kZoomStep);
        dock.fitAction()->trigger();
        QCOMPARE(tp->body().scaleMode, std::string("fit"));

        // Another worksheet: a new embedded widget over it, the dock's settings kept.
        cmptest::World other;
        dock.setContext(other.context.get());
        QVERIFY(dock.trackPosition() != nullptr);
        QCOMPARE(dock.context(), other.context.get());
        QVERIFY(dock.trackPosition()->body().showSectionLabels);
        QVERIFY(!dock.trackPosition()->body().showSectionColours);
        // No worksheet: the note again.
        dock.setContext(nullptr);
        QVERIFY(dock.trackPosition() == nullptr);
        QVERIFY(note->isVisible());
        // The actions still edit the settings without a map shown.
        dock.showSectionsAction()->trigger();
        QVERIFY(dock.showSectionsAction()->isChecked());
    }
};

QTEST_MAIN(TstTrackMapWindow)
#include "tst_track_map_window.moc"
