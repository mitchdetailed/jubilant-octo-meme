// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackPosition (04 section 14, 04 section 7.4; 06 sections 9 and 14.2), offscreen: the venue's map bound by
// the Main source through RenderService and TrackMapService, the "no track map" banner and a map arriving
// later, Dynamic-only rebuilds on cursor, zoom and domain changes (never a Static build), the cursor dots,
// PLUS / MINUS / "=" and the track.* commands through the CommandRegistry and the KeymapBinder (key context
// Track), the wheel zooming about the pointer, a drag panning, a double-click fitting, the gesture committed
// as one spec edit, a backgrounded map not consuming clicks, and Print to Clipboard equal to
// OffscreenRenderer at the same inputs. The map is a hand-made rectangle of 2000 m, the source's laps are
// driven at constant speeds of exactly 2000 m per lap (report_track_fixtures.hpp), so every track position is
// known in closed form.

#include "components/reports_track/rt_support.hpp"

#include <esses/project/keymap.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/widgets/components/track_position.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::rtw;

namespace {

constexpr Micros kSec = 1'000'000;
const std::string kVenue = "Rect Venue";

struct TrackWorld {
    World w;
    qt::TrackMapService maps{nullptr};
    std::shared_ptr<const model::DataSource> src;

    explicit TrackWorld(bool withMap = true) {
        w.render.setTrackMaps(&maps);
        if (withMap) {
            static_cast<void>(
                maps.setMap(kVenue, std::make_shared<const analysis::TrackMap>(rectMap(kVenue))));
        }
        src = publish(w, linearSource("tp-src", kVenue, {40.0, 50.0, 50.0, 30.0}, 2000.0, *shippedUnits(),
                                      w.rt.cache()));
        w.selection.setMain(lapRef(*src, 1));
    }
    bool distanceReady() { return w.distanceReady(src); }
};

project::ComponentSpec trackSpec(const QJsonObject& extra = {}) {
    QJsonObject body{{QStringLiteral("showZoomSpan"), true},
                     {QStringLiteral("showSectionLabels"), false},
                     {QStringLiteral("showSectionMarks"), true},
                     {QStringLiteral("showSectionColours"), true}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        body.insert(it.key(), it.value());
    }
    return componentSpec(TrackPosition::kType, body);
}

bool built(const TrackPosition& tp) {
    return QTest::qWaitFor([&] { return tp.staticUpToDate(); }, 20000);
}

// The track area as plot::TrackGeometry lays it out (caption 18 px, 6 px inset; logical = device at DPR 1).
plot::RectPx mapArea(const QWidget& widget, bool caption = true) {
    const float top = (caption ? 18.0F : 0.0F) + 6.0F;
    return plot::RectPx{6.0F, top, static_cast<float>(widget.width()) - 12.0F,
                        static_cast<float>(widget.height()) - top - 6.0F};
}

int countNear(const QImage& image, QRgb want, int tolerance = 24) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (std::abs(qRed(line[x]) - qRed(want)) <= tolerance &&
                std::abs(qGreen(line[x]) - qGreen(want)) <= tolerance &&
                std::abs(qBlue(line[x]) - qBlue(want)) <= tolerance) {
                ++n;
            }
        }
    }
    return n;
}

QRgb rgbOf(project::Rgba c) {
    return qRgb(static_cast<int>((c >> 24) & 0xFFU), static_cast<int>((c >> 16) & 0xFFU),
                static_cast<int>((c >> 8) & 0xFFU));
}

// As the shell does (main_window_m3.cpp): the Track rows are registered with a handler that offers the
// command to the focused component, enabled while that component's key context is Track.
void registerTrackCommands(qt::CommandRegistry& registry, ComponentWidget& focused) {
    for (const char* id : {"track.zoom_in", "track.zoom_out", "track.fit"}) {
        qt::CommandSpec s;
        s.id = QString::fromLatin1(id);
        s.context = qt::CommandContext::Track;
        const QString command = s.id;
        s.invoke = [&focused, command](const QVariantList& args) { focused.handleCommand(command, args); };
        s.enabled = [&focused] {
            return focused.isFocusedComponent() && focused.commandContext() == qt::CommandContext::Track;
        };
        registry.add(std::move(s));
    }
}

} // namespace

class TstTrackPosition : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void drawsTheVenueMapWithTheCursorDots() {
        TrackWorld t;
        std::unique_ptr<ComponentWidget> owner(ComponentWidget::create(trackSpec(), *t.w.context));
        auto* tp = qobject_cast<TrackPosition*>(owner.get());
        QVERIFY(tp != nullptr);
        QCOMPARE(tp->overlayPolicy(), ComponentWidget::OverlayPolicy::AllOverlays);
        QCOMPARE(tp->commandContext(), qt::CommandContext::Track);
        tp->resize(400, 300);
        tp->show();
        QVERIFY(QTest::qWaitFor([&] { return t.distanceReady(); }, 20000));
        tp->onDataChanged(t.src->id, 0);
        // Settled, not only built: the other laps' distance indexes (each a Data change) are in, so nothing
        // can rebuild the map behind the checks below.
        QVERIFY(QTest::qWaitFor([&] { return settled(*tp); }, 20000));
        QVERIFY(tp->trackMap() != nullptr);
        QCOMPARE(tp->trackMap()->venue, kVenue);
        QVERIFY(tp->staticDelivered() >= 1);
        // The centreline in the scheme's trackLine colour, the Main dot in cursorMain.
        const QImage img = tp->renderToImage(1.0);
        QVERIFY(countNear(img, rgbOf(t.w.scheme->role("trackLine", 0x606060FFU)), 8) > 200);
        // The cursor at 0 (the Main lap's start, a BCN): the dot sits at the start line, the middle of the
        // bottom side: x = 350 of 700 m, y = 0.
        const plot::TrackFit fit = plot::TrackGeometry::fit(*tp->trackMap(), mapArea(*tp), tp->body());
        const plot::PointPx start = fit.toPx(350.0, 0.0);
        const QRgb red = rgbOf(t.w.scheme->role("cursorMain", 0xFF0000FFU));
        const QImage dot = img.copy(static_cast<int>(start.x) - 4, static_cast<int>(start.y) - 4, 9, 9);
        QVERIFY(countNear(dot, red, 40) >= 4);

        // Cursor, zoom and domain changes rebuild the Dynamic layer only (04 section 7.4).
        const int statics = tp->staticSubmitted();
        const int dynamics = tp->dynamicBuilds();
        t.w.view->setCursor(25 * kSec, qt::CursorSource::Programmatic); // half a 50 s lap: 1000 m on
        QVERIFY(tp->dynamicBuilds() > dynamics);
        t.w.view->zoomTo(plot::XSpan{0.0, 10.0 * kSec});
        t.w.view->setDomain(plot::Domain::Distance);
        QVERIFY(tp->staticUpToDate()); // a Static invalidation would show at once
        drainEvents();
        QCOMPARE(tp->staticSubmitted(), statics);
        QVERIFY(tp->staticUpToDate()); // nothing scheduled either
        // 1000 m from the start line: 350 + 300 + 350 -> the top side's middle, x = 350, y = 300.
        const QImage moved = tp->renderToImage(1.0);
        const plot::PointPx half = fit.toPx(350.0, 300.0);
        QVERIFY(countNear(moved.copy(static_cast<int>(half.x) - 4, static_cast<int>(half.y) - 4, 9, 9), red,
                          40) >= 4);
        QVERIFY(countNear(moved.copy(static_cast<int>(start.x) - 4, static_cast<int>(start.y) - 4, 9, 9), red,
                          40) < 4);
        // The zoom span is drawn in cursorZoom.
        QVERIFY(countNear(moved, rgbOf(t.w.scheme->role("cursorZoom", 0x3080FFFFU)), 8) > 20);
    }

    void bannerWithoutAMapThenTheMapArrives() {
        TrackWorld t(false);
        TrackPosition tp(trackSpec(), *t.w.context);
        tp.resize(300, 200);
        tp.show();
        QVERIFY(built(tp));
        QVERIFY(tp.trackMap() == nullptr);
        QVERIFY(tp.layers().chromeList().text.find("No track map") != std::string::npos);
        // The Track Editor writes the map: the component follows (TrackMapService::mapChanged).
        const int before = tp.staticDelivered();
        QVERIFY(t.maps.setMap(kVenue, std::make_shared<const analysis::TrackMap>(rectMap(kVenue))).ok());
        QVERIFY(QTest::qWaitFor([&] { return tp.staticDelivered() > before && tp.staticUpToDate(); }, 20000));
        QVERIFY(tp.trackMap() != nullptr);
        QVERIFY(tp.layers().chromeList().text.find("No track map") == std::string::npos);
    }

    void keysAndCommandsZoomAboutTheCentre() {
        TrackWorld t;
        TrackPosition tp(trackSpec(), *t.w.context);
        tp.resize(400, 300);
        tp.show();
        QVERIFY(built(tp));
        registerTrackCommands(t.w.commands, tp);
        QSignalSpy edits(&tp, &ComponentWidget::specEditRequested);
        // Not the worksheet's focused component: the shell's rows stay disabled.
        QVERIFY(!t.w.commands.invoke(QStringLiteral("track.zoom_in")));
        tp.setFocusedComponent(true);
        QVERIFY(t.w.commands.invoke(QStringLiteral("track.zoom_in")));
        QCOMPARE(tp.body().scaleMode, std::string("manual"));
        QCOMPARE(tp.body().zoom, TrackPosition::kZoomStep);
        QCOMPARE(tp.body().panX, 0.0); // about the centre: no pan
        // The KeymapBinder resolves "+" in the Track context to track.zoom_in (06 section 14.2).
        std::vector<std::string> log;
        auto keymap = project::Keymap::load(repoRoot() / "data" / "keymaps" / "default.json", log);
        QVERIFY(keymap.ok());
        qt::KeymapBinder binder(t.w.commands);
        binder.setKeymap(*keymap);
        const QList<qt::CommandContext> chain{qt::CommandContext::Track, qt::CommandContext::Component,
                                              qt::CommandContext::Worksheet, qt::CommandContext::Global};
        QVERIFY(binder.dispatch(QStringLiteral("+"), chain));
        QCOMPARE(tp.body().zoom, TrackPosition::kZoomStep * TrackPosition::kZoomStep);
        QVERIFY(binder.dispatch(QStringLiteral("-"), chain));
        QCOMPARE(tp.body().zoom, TrackPosition::kZoomStep);
        // Keys straight to the widget (the dock, or no shell): +, = and -.
        QTest::keyClick(&tp, Qt::Key_Equal);
        QCOMPARE(tp.body().zoom, TrackPosition::kZoomStep * TrackPosition::kZoomStep);
        QTest::keyClick(&tp, Qt::Key_Minus);
        QTest::keyClick(&tp, Qt::Key_Minus);
        QCOMPARE(tp.body().zoom, 1.0);
        QTest::keyClick(&tp, Qt::Key_Plus);
        // A keymap that binds track.zoom_in to Z and nothing to the old keys (D-203 (o)): "=" does nothing
        // any more, Z zooms; without the shell's provider the defaults are back.
        const double before = tp.body().zoom;
        ComponentWidget::setKeysProvider([](const QString& id) {
            return id == QLatin1String("track.zoom_in") ? QStringList{QStringLiteral("Z")} : QStringList{};
        });
        QTest::keyClick(&tp, Qt::Key_Equal);
        QTest::keyClick(&tp, Qt::Key_Minus);
        QCOMPARE(tp.body().zoom, before);
        QTest::keyClick(&tp, Qt::Key_Z);
        QCOMPARE(tp.body().zoom, before * TrackPosition::kZoomStep);
        ComponentWidget::setKeysProvider({});
        QTest::keyClick(&tp, Qt::Key_Minus);
        QCOMPARE(tp.body().zoom, before);
        // The gesture is one spec edit once it rests.
        QVERIFY(edits.wait(2000));
        QCOMPARE(edits.count(), 1);
        QCOMPARE(edits.at(0).at(1).toString(), QStringLiteral("Zoom track view"));
        const auto next = edits.at(0).at(0).value<project::ComponentSpec>();
        const QJsonObject scale = objectOf(next.body).value(QStringLiteral("scale")).toObject();
        QCOMPARE(scale.value(QStringLiteral("mode")).toString(), QStringLiteral("manual"));
        QCOMPARE(scale.value(QStringLiteral("zoom")).toDouble(), TrackPosition::kZoomStep);
        // The canvas applies it: nothing is rebuilt again for the same spec (counted once nothing else, such
        // as a lap's distance index, can still rebuild the map).
        QVERIFY(QTest::qWaitFor([&] { return settled(tp); }, 20000));
        const int submitted = tp.staticSubmitted();
        tp.applySpec(next);
        QVERIFY(tp.staticUpToDate());
        drainEvents();
        QCOMPARE(tp.staticSubmitted(), submitted);
        // track.fit: back to "fit", committed at once.
        QVERIFY(t.w.commands.invoke(QStringLiteral("track.fit")));
        QCOMPARE(tp.body().scaleMode, std::string("fit"));
        QCOMPARE(edits.count(), 2);
        QVERIFY(tp.handleCommand(QStringLiteral("track.zoom_out"), {}));
        QVERIFY(!tp.handleCommand(QStringLiteral("zoom.v_in"), {})); // not a track command
        // Another component focused: the rows go quiet.
        tp.setFocusedComponent(false);
        QVERIFY(!t.w.commands.invoke(QStringLiteral("track.zoom_in")));
        QVERIFY(tp.contextCommands().contains(QStringLiteral("track.fit")));
    }

    void wheelZoomsAboutThePointerAndDragPans() {
        TrackWorld t;
        TrackPosition tp(trackSpec(), *t.w.context);
        tp.resize(400, 300);
        tp.show();
        QVERIFY(built(tp));
        tp.setFocusedComponent(true);
        const QPointF pointer(120.0, 200.0);
        const plot::TrackFit before = plot::TrackGeometry::fit(*tp.trackMap(), mapArea(tp), tp.body());
        // The map point under the pointer (in the rotated frame, which is the map frame here: no rotation).
        const double mx = (pointer.x() - before.offsetX) / before.scale;
        const double my = (before.offsetY - pointer.y()) / before.scale;
        QWheelEvent wheel(pointer, tp.mapToGlobal(pointer), QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&tp, &wheel);
        QVERIFY(wheel.isAccepted());
        QCOMPARE(tp.body().zoom, TrackPosition::kZoomStep);
        const plot::TrackFit after = plot::TrackGeometry::fit(*tp.trackMap(), mapArea(tp), tp.body());
        const plot::PointPx q = after.toPx(mx, my);
        QVERIFY(std::abs(static_cast<double>(q.x) - pointer.x()) < 0.5);
        QVERIFY(std::abs(static_cast<double>(q.y) - pointer.y()) < 0.5);

        // A drag of (40, -20) px moves the map with the pointer: 40 px right, 20 px up.
        const plot::PointPx origin = after.toPx(350.0, 0.0);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(200, 150), tp.mapToGlobal(QPointF(200, 150)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tp, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(240, 130), tp.mapToGlobal(QPointF(240, 130)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tp, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(240, 130), tp.mapToGlobal(QPointF(240, 130)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&tp, &release);
        const plot::TrackFit panned = plot::TrackGeometry::fit(*tp.trackMap(), mapArea(tp), tp.body());
        const plot::PointPx moved = panned.toPx(350.0, 0.0);
        QVERIFY(std::abs(moved.x - (origin.x + 40.0F)) < 0.5F);
        QVERIFY(std::abs(moved.y - (origin.y - 20.0F)) < 0.5F);
        // A double-click fits.
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(200, 150), tp.mapToGlobal(QPointF(200, 150)),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tp, &dbl);
        QCOMPARE(tp.body().scaleMode, std::string("fit"));
        // Not focused: the wheel is not taken.
        tp.setFocusedComponent(false);
        tp.clearFocus();
        QWheelEvent ignored(pointer, tp.mapToGlobal(pointer), QPoint(), QPoint(0, 120), Qt::NoButton,
                            Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&tp, &ignored);
        QVERIFY(!ignored.isAccepted());
        QCOMPARE(tp.body().scaleMode, std::string("fit"));
    }

    void aBackgroundMapDoesNotConsumeClicks() {
        TrackWorld t;
        TrackPosition tp(trackSpec(QJsonObject{{QStringLiteral("background"), true}}), *t.w.context);
        tp.resize(300, 200);
        tp.show();
        QVERIFY(tp.body().background);
        QSignalSpy focus(&tp, &ComponentWidget::focusRequested);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(150, 100), tp.mapToGlobal(QPointF(150, 100)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tp, &press);
        QVERIFY(!press.isAccepted());
        QCOMPARE(focus.count(), 0);
    }

    void printToClipboardEqualsTheOffscreenRenderer() {
        TrackWorld t;
        t.w.selection.toggleOverlay0(lapRef(*t.src, 2));
        TrackPosition tp(trackSpec(QJsonObject{{QStringLiteral("showSectionLabels"), true}}), *t.w.context);
        tp.resize(360, 260);
        tp.show();
        QVERIFY(QTest::qWaitFor([&] { return t.distanceReady(); }, 20000));
        tp.onDataChanged(t.src->id, 0);
        t.w.view->setCursor(12 * kSec, qt::CursorSource::Programmatic);
        QVERIFY(built(tp));
        const QImage widget = tp.renderToImage(1.0);
        qt::OffscreenRenderer renderer(std::make_shared<const qt::SchemeFonts>(*t.w.scheme));
        plot::ComponentBuildInputs in = tp.inputs();
        in.view = t.w.view->state();
        const QImage offscreen = renderer.renderComponent(in, QSize(360, 260), 1.0);
        QCOMPARE(widget.size(), offscreen.size());
        const QImage a = widget.convertToFormat(QImage::Format_ARGB32);
        const QImage b = offscreen.convertToFormat(QImage::Format_ARGB32);
        int differ = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y);
                const QRgb r = b.pixel(x, y);
                if (std::abs(qRed(p) - qRed(r)) > 8 || std::abs(qGreen(p) - qGreen(r)) > 8 ||
                    std::abs(qBlue(p) - qBlue(r)) > 8) {
                    ++differ;
                }
            }
        }
        QVERIFY2(differ <= a.width() * a.height() / 200,
                 qPrintable(QStringLiteral("%1 pixels differ").arg(differ)));
        // At another DPR: the size doubles (Static scaled, Dynamic and Chrome rebuilt).
        QCOMPARE(tp.renderToImage(2.0).size(), QSize(720, 520));
    }
};

QTEST_MAIN(TstTrackPosition)
#include "tst_track_position.moc"
