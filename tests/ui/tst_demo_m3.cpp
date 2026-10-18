// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 demo script (07 section 6.5), offscreen, in the real shell (MainWindow on a private configuration
// directory, the shipped circuit-default workbook), every key through the KeyEventRouter:
//   Open the "Braking" worksheet; Main + Overlay 0 + two overlays from the Data window (ENTER, CTRL+ENTER,
//   ALT+ENTER); F3: the variance panel with one trace per overlay in its key colour; D, SPACE: the datum, the
//   key's value@datum and delta columns; M: the measurements, a hover line over a max cell; O: the offset
//   axis, ALT+SHIFT+RIGHT x4 nudges the reference overlay (one undo step); Tools > Track Editor: Generate
//   from GPS, Accept (default sections), add "T1 entry"; the "Reports" worksheet: the Channel Report (Laps
//   scope, headings Venue > Driver > Lap, avg/min/max, delta vs Overlay 0) copied as TSV, the Time Report
//   with section times, Eclectic and Rolling minimum rows, ALT+3 colours the 3 % band; back on "Braking": the
//   scatter of Throttle vs Brake with its polynomial-3 trend equation, a click on a point moves the cursor; A
//   animates at x10 with Cursor-only repaints; CTRL+SHIFT+P "single colour" ENTER toggles single colour.
// The log is written by the test: the AC-M3-04 synthetic circuit's true centreline (fixtures/track/
// synthetic-circuit/expected.centre.csv, about 2975 m) driven by an out lap, flying laps of 70, 68, 69 and 67
// s and an in lap, each lap the same profile in lap fraction u (speed L/P (1 + 0.3 sin 2 pi u), so a section
// takes a fixed share of every lap); GPS from the centreline, throttle 100 max(0, cos 2 pi u), brake 100
// max(0, -cos 2 pi u), the G forces and steering from the speed and the centreline's curvature. Expected
// values come from these formulas (D-041), never from the code under test.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "../../libs/qtmodel/tests/reports/report_track_fixtures.hpp"
#include "ui_test_support.hpp"

#include <esses/analysis/track_map.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/model/marker.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/scatter_plot.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

// The qtmodel synthetic-source helpers (.ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

constexpr int kTimeoutMs = 90000;
constexpr double kG = 9.80665;
constexpr unsigned kRateHz = 50;
constexpr unsigned kGpsHz = 10;
const std::vector<double> kLapSeconds{85.0, 70.0, 68.0, 69.0, 67.0, 90.0}; // out, four flying, in
const QString kVenue = QStringLiteral("Demo Circuit");

// ---- the demo log
// -------------------------------------------------------------------------------------------

struct Circuit {
    double lengthM = 0.0;
    std::vector<std::pair<double, double>> centre; // ENU m, every 1 m, closed
    double lat0 = 0.0;
    double lon0 = 0.0;

    std::pair<double, double> at(double s) const {
        const double n = static_cast<double>(centre.size() - 1);
        double x = std::fmod(s, lengthM);
        if (x < 0.0) {
            x += lengthM;
        }
        const double pos = x / lengthM * n;
        const auto i = std::min(static_cast<std::size_t>(pos), centre.size() - 2);
        const double f = pos - static_cast<double>(i);
        return {centre[i].first + f * (centre[i + 1].first - centre[i].first),
                centre[i].second + f * (centre[i + 1].second - centre[i].second)};
    }
    double heading(double s) const {
        const auto a = at(s - 0.5);
        const auto b = at(s + 0.5);
        return std::atan2(b.second - a.second, b.first - a.first);
    }
    // Curvature (1/m, positive turning left) over a 10 m baseline.
    double curvature(double s) const {
        double d = heading(s + 5.0) - heading(s - 5.0);
        while (d > std::numbers::pi) {
            d -= 2.0 * std::numbers::pi;
        }
        while (d < -std::numbers::pi) {
            d += 2.0 * std::numbers::pi;
        }
        return d / 10.0;
    }
};

Circuit readCircuit() {
    const std::filesystem::path dir = qt::test::repoRoot() / "fixtures" / "track" / "synthetic-circuit";
    const qt::rttest::CircuitTruth truth = qt::rttest::readCircuitTruth(dir);
    Circuit c;
    c.lengthM = truth.lengthM;
    c.centre = truth.centre;
    c.lat0 = truth.originLat;
    c.lon0 = truth.originLon;
    return c;
}

// The lap and its fraction at time t (seconds from the log start).
struct LapPhase {
    std::size_t lap = 0;
    double u = 0.0;
    double period = 0.0;
};

LapPhase phaseAt(double t) {
    double start = 0.0;
    for (std::size_t k = 0; k < kLapSeconds.size(); ++k) {
        if (t < start + kLapSeconds[k] || k + 1 == kLapSeconds.size()) {
            return {k, std::min(1.0, (t - start) / kLapSeconds[k]), kLapSeconds[k]};
        }
        start += kLapSeconds[k];
    }
    return {};
}

double throttleAt(double u) {
    return 100.0 * std::max(0.0, std::cos(2.0 * std::numbers::pi * u));
}

std::shared_ptr<const model::DataSource> demoSource(const Circuit& c, store::ChunkCache& cache) {
    const units::UnitRegistry& units = *qt::test::shippedUnits();
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view("demo-m3.ld"));
    src->prov.path = "demo-m3.ld";
    src->prov.importerId = "motec.ld";
    src->prov.hash = src->id;
    double total = 0.0;
    for (const double s : kLapSeconds) {
        total += s;
    }
    const auto n = static_cast<std::size_t>(std::llround(total * kRateHz));
    std::vector<float> fl(n), fr(n), throttle(n), brake(n), latG(n), longG(n), steer(n);
    const double L = c.lengthM;
    const auto distanceIn = [L](double u) {
        return L * (u + 0.3 / (2.0 * std::numbers::pi) * (1.0 - std::cos(2.0 * std::numbers::pi * u)));
    };
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRateHz;
        const LapPhase p = phaseAt(t);
        const double w = 2.0 * std::numbers::pi * p.u;
        const double v = L / p.period * (1.0 + 0.3 * std::sin(w));
        const double a = L / p.period * 0.3 * (2.0 * std::numbers::pi / p.period) * std::cos(w);
        const double kappa = c.curvature(distanceIn(p.u));
        fl[i] = static_cast<float>(v);
        fr[i] = static_cast<float>(0.97 * v);
        throttle[i] = static_cast<float>(throttleAt(p.u));
        brake[i] = static_cast<float>(100.0 * std::max(0.0, -std::cos(w)));
        latG[i] = static_cast<float>(v * v * kappa / kG);
        longG[i] = static_cast<float>(a / kG);
        steer[i] = static_cast<float>(kappa * 2.7 * 14.0 * 180.0 / std::numbers::pi);
    }
    const auto ng = static_cast<std::size_t>(std::llround(total * kGpsHz));
    std::vector<double> lat(ng);
    std::vector<double> lon(ng);
    constexpr double kMetresPerDegree = 111'320.0;
    for (std::size_t i = 0; i < ng; ++i) {
        const LapPhase p = phaseAt(static_cast<double>(i) / kGpsHz);
        const auto [x, y] = c.at(distanceIn(p.u));
        lat[i] = c.lat0 + y / kMetresPerDegree;
        lon[i] = c.lon0 + x / (kMetresPerDegree * std::cos(c.lat0 * std::numbers::pi / 180.0));
    }
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = static_cast<Micros>(std::llround(total * 1e6));
    const Rate rate{kRateHz, 1};
    const auto add = [&](const char* name, const char* unit, std::vector<float> values, int dps) {
        layer.addChannel(qt::rttest::uniformChannel(src->id, name, *units.find(unit), units, rate,
                                                    std::move(values), cache, dps));
    };
    add("Wheel Speed FL", "m/s", fl, 2);
    add("Wheel Speed FR", "m/s", fr, 2);
    add("THROTTLE", "%", throttle, 1);
    add("BRAKE", "%", brake, 1);
    add("G Force Lat", "G", latG, 2);
    add("G Force Long", "G", longG, 2);
    add("STEERANGLE", "deg", steer, 1);
    const auto gps = [&](const char* name, std::vector<double> values) {
        model::Channel ch = qt::rttest::uniformChannel(src->id, name, *units.find("deg"), units,
                                                       Rate{kGpsHz, 1}, {}, cache, 7);
        store::ColumnOptions co;
        co.cache = &cache;
        ch.desc.dtype = store::Dtype::F64;
        ch.series = store::UniformSeries{
            Rate{kGpsHz, 1}, 0,
            store::Column(store::InMemoryProvider::fromF64(values, {}, std::string("memory:") + name), co)};
        layer.addChannel(std::move(ch));
    };
    gps("GPS Latitude", lat);
    gps("GPS Longitude", lon);
    layer.markers.push_back(
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived));
    double boundary = 0.0;
    for (std::size_t k = 0; k + 1 < kLapSeconds.size(); ++k) {
        boundary += kLapSeconds[k];
        layer.markers.push_back(model::makeLoggedMarker(
            model::MarkerClass::BCN, static_cast<Micros>(std::llround(boundary * 1e6)), "Beacons", "Beacon",
            {}, 77, 0, model::Provenance::Companion));
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, layer.duration, "", "", "", 0, 0,
                                                    model::Provenance::Derived));
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    src->layers.push_back(std::move(layer));
    src->details.push_back(qt::rttest::textDetail("Venue", kVenue.toStdString()));
    src->details.push_back(qt::rttest::textDetail("Driver", "Demo Driver"));
    src->baseDetails = src->details;
    analysis::RuleRunner::circuitDefault().run(*src);
    return src;
}

// ---- shell helpers
// ------------------------------------------------------------------------------------------

int worksheetNamed(MainWindow& w, const QString& name) {
    for (int i = 0; i < w.worksheetTabs().count(); ++i) {
        if (w.worksheetTabs().tabText(i) == name) {
            return i;
        }
    }
    return -1;
}

template <class T> T* componentOf(MainWindow& w) {
    for (ComponentWidget* c : w.canvas().components()) {
        if (auto* t = qobject_cast<T*>(c)) {
            return t;
        }
    }
    return nullptr;
}

void focusComponent(MainWindow& w, ComponentWidget* c) {
    QVERIFY(c != nullptr);
    w.canvas().focusComponent(c);
    c->setFocus(Qt::OtherFocusReason);
}

void pressIn(MainWindow& w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    uitest::press(w, key, mods);
}

double wide(float v) {
    return static_cast<double>(v);
}

// The numbers the text runs of a DrawList show ("+12.3" and "245.07" alike).
std::vector<double> numbersShown(const plot::DrawList& list) {
    std::vector<double> out;
    for (const plot::TextRun& r : list.texts) {
        const QString text = QString::fromStdString(list.text.substr(r.byteOffset, r.byteLen)).trimmed();
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (ok) {
            out.push_back(v);
        }
    }
    return out;
}

bool shows(const std::vector<double>& numbers, double want, double tolerance) {
    return std::any_of(numbers.begin(), numbers.end(),
                       [&](double v) { return std::abs(v - want) <= tolerance; });
}

// The colours of the Polyline commands whose first vertex lies inside `r` (device px).
std::set<project::Rgba> polylineColoursIn(const plot::DrawList& list, const plot::RectPx& r) {
    std::set<project::Rgba> out;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind != plot::CmdKind::Polyline || c.vertexCount < 2) {
            continue;
        }
        const float x = list.verts[2 * c.vertexOffset];
        const float y = list.verts[2 * c.vertexOffset + 1];
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
            out.insert(c.colour);
        }
    }
    return out;
}

const model::RangeGroup* groupOf(MainWindow& w, const model::SourceId& src, std::string_view path) {
    const auto s = w.selection().source(src);
    return s == nullptr || s->layers.empty() ? nullptr : s->layers.front().ranges.find(path);
}

QJsonObject bodyOf(const ComponentWidget& c) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(c.spec().body.compact())).object();
}

} // namespace

class TstDemoM3 : public QObject {
    Q_OBJECT
public:
    static void initMain() { uitest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    void cleanup() { QCOMPARE(uitest::assertFailures(), 0); }

    // 06 section 9 / 13.5 item 6 (D-027; D-203 (u)): a log of a venue without a map generates it on open
    // (GPS here), as an automatic step (no undo command) announced in Messages, unless the project option is
    // off; the Track Editor's "Add from Cursor Span" takes the datum-to-cursor span in track metres.
    void trackMapOnOpenAndTheCursorSpan() {
        QTemporaryDir config;
        QTemporaryDir logs;
        QVERIFY(config.isValid() && logs.isValid());
        auto rt = uitest::makeRuntime();
        const Circuit circuit = readCircuit();
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal);
            ld = qt::test::writeLd(*demoSource(circuit, rt->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "demo-m3");
        }
        {
            // The option off: nothing is generated.
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(uitest::showAndActivate(w));
            w.trackEditor().setAutoGenerateOnOpen(false);
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, kTimeoutMs);
            // A generation started at the open would have run and been delivered by the time the runtime is
            // quiet (a fixed wait proves nothing on a loaded machine).
            QTRY_VERIFY_WITH_TIMEOUT(uitest::runtimeQuiet(*rt), kTimeoutMs);
            QVERIFY(w.trackMaps().mapFor(*w.selection().source(src)) == nullptr);
        }
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(uitest::showAndActivate(w));
            QVERIFY(w.trackEditor().autoGenerateOnOpen()); // the default
            const int messages = w.messages().rowCount();
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_VERIFY_WITH_TIMEOUT(w.trackMaps().mapFor(*w.selection().source(src)) != nullptr, kTimeoutMs);
            const auto map = w.trackMaps().mapFor(*w.selection().source(src));
            QVERIFY(std::abs(map->lengthM - circuit.lengthM) < 0.02 * circuit.lengthM);
            QVERIFY(!map->sections.empty());              // the default sections came with it
            QCOMPARE(w.session().undoStack().count(), 0); // automatic: nothing to undo
            bool told = false;
            for (int r = messages; r < w.messages().rowCount(); ++r) {
                told = told || w.messages().at(r).text.contains(QStringLiteral("Generated the track map"));
            }
            QVERIFY(told);

            // The cursor span: Main a flying lap, a datum at 10 s, the cursor at 20 s.
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, kTimeoutMs);
            const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src);
            QVERIFY(laps.size() >= 3);
            w.selection().setMain(laps[1]);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            QVERIFY(w.commands().invoke(QStringLiteral("tools.track_editor")));
            TrackEditor& editor = w.trackEditor();
            QVERIFY(!editor.addSectionFromCursorSpan(QStringLiteral("No datum"))); // no span yet
            ws.setCursor(10 * 1'000'000, qt::CursorSource::Programmatic);
            ws.placeDatum();
            ws.setCursor(20 * 1'000'000, qt::CursorSource::Programmatic);
            QVERIFY(editor.setLap(laps[1]));
            QTRY_VERIFY(editor.addSectionFromCursorSpan(QStringLiteral("Span")));
            const analysis::TrackSection* span = nullptr;
            for (const analysis::TrackSection& sec : editor.currentMap()->sections) {
                if (sec.name == "Span") {
                    span = &sec;
                }
            }
            QVERIFY(span != nullptr);
            QVERIFY(span->endM - span->startM > 10.0);           // ten seconds of a lap at speed
            QVERIFY(span->endM <= editor.currentMap()->lengthM); // in track metres
            editor.close();
        }
        rt->shutdown();
    }

    void demoScript() {
        QTemporaryDir config;
        QTemporaryDir logs;
        QVERIFY(config.isValid() && logs.isValid());
        auto rt = uitest::makeRuntime();
        const Circuit circuit = readCircuit();
        QVERIFY(circuit.lengthM > 2900.0 && circuit.centre.size() > 2900);
        std::filesystem::path ld;
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing the log reads every chunk
            ld = qt::test::writeLd(*demoSource(circuit, rt->cache()),
                                   std::filesystem::path(logs.path().toStdWString()), "demo-m3");
        }
        {
            MainWindow w(*rt, uitest::pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(uitest::showAndActivate(w));
            w.openLog(ld);
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().size() == 1, kTimeoutMs);
            const model::SourceId src = w.selection().sources().front()->id;
            QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(src), qt::DistanceState::Ready, kTimeoutMs);
            const std::vector<model::RangeRef> laps = w.selection().displayedRanges(src);
            QCOMPARE(laps.size(), kLapSeconds.size());
            QUndoStack& stack = w.session().undoStack();
            const std::shared_ptr<const project::Scheme> scheme = w.session().scheme();
            QVERIFY(scheme != nullptr);

            // ---- Open the "Braking" worksheet; Main (67 s) + Overlay 0 (70 s) + two overlays (68 s, 69 s).
            const int braking = worksheetNamed(w, QStringLiteral("Braking"));
            QVERIFY(braking >= 0);
            w.selectWorksheet(braking);
            QCOMPARE(w.activeWorksheetIndex(), braking);
            qt::WorksheetViewModel& ws = *w.activeWorksheet();
            QCOMPARE(ws.domain(), plot::Domain::Distance);
            const model::RangeRef mainLap = laps[4];
            const model::RangeRef zeroLap = laps[1];
            pressIn(w, Qt::Key_F8); // the Data window
            QCOMPARE(w.docks().focusedDockId(), QStringLiteral("data"));
            w.dataWindow().highlight(mainLap);
            pressIn(w, Qt::Key_Return); // Main
            w.dataWindow().highlight(zeroLap);
            pressIn(w, Qt::Key_Return, Qt::ControlModifier); // Overlay 0
            w.dataWindow().highlight(laps[2]);
            pressIn(w, Qt::Key_Return, Qt::AltModifier); // Overlay 1
            w.dataWindow().highlight(laps[3]);
            pressIn(w, Qt::Key_Return, Qt::AltModifier); // Overlay 2
            const plot::SelectionSnapshot sel = w.selection().snapshot();
            QVERIFY(sel.main == mainLap);
            QVERIFY(sel.overlay0 == zeroLap);
            QVERIFY((sel.overlays == std::vector<model::RangeRef>{laps[2], laps[3]}));

            TimeDistanceGraph* g = uitest::graphOf(w);
            QVERIFY(g != nullptr);
            plot::DrawList lastStatic;
            g->setBuildObserver([&lastStatic](const plot::ComponentBuild& b) { lastStatic = b.list; });
            focusComponent(w, g);
            QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));

            // ---- F3: the variance panel, one trace per overlay in its key colour (06 section 5.3 item 3).
            pressIn(w, Qt::Key_F3);
            QTRY_VERIFY_WITH_TIMEOUT(g->layout().variance.h > 0.0F, kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
            QCOMPARE(g->staticInfo().varianceTables, 3);
            QVERIFY(!g->staticInfo().varianceNoData);
            const std::set<project::Rgba> varianceColours =
                polylineColoursIn(lastStatic, g->layout().variance);
            for (const project::Rgba key :
                 {scheme->role("overlay0"), scheme->overlayPalette.at(0), scheme->overlayPalette.at(1)}) {
                QVERIFY2(
                    varianceColours.count(key) == 1,
                    qPrintable(QStringLiteral("no variance trace in #%1").arg(key, 8, 16, QLatin1Char('0'))));
            }

            // ---- D, SPACE: the datum; the key gains value@datum and delta.
            ws.setCursor(10 * 1'000'000, qt::CursorSource::Programmatic);
            pressIn(w, Qt::Key_D);
            QVERIFY(w.workspace().datumT().has_value());
            QTRY_VERIFY_WITH_TIMEOUT(g->layout().keyColumns.datum, kTimeoutMs);
            ws.setCursor(20 * 1'000'000, qt::CursorSource::Programmatic);
            pressIn(w, Qt::Key_Space);
            QVERIFY(w.workspace().datumT() == std::optional<Micros>(20 * 1'000'000));
            QVERIFY(g->layout().keyColumns.datum);
            {
                // The key's Corr Speed value@datum and delta against the log's closed form: Main is the 67 s
                // lap, the datum 20 s into it (Corr Speed = the faster wheel, FL, in km/h). Move the cursor
                // to 10 s: the delta is v(10 s) - v(20 s).
                const double period = kLapSeconds[4];
                const auto kmh = [&](double t) {
                    return 3.6 * circuit.lengthM / period *
                           (1.0 + 0.3 * std::sin(2.0 * std::numbers::pi * t / period));
                };
                ws.setCursor(10 * 1'000'000, qt::CursorSource::Programmatic);
                QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
                QTRY_VERIFY_WITH_TIMEOUT(shows(numbersShown(g->layers().dynamicList()), kmh(20.0), 0.06),
                                         kTimeoutMs);
                QTRY_VERIFY_WITH_TIMEOUT(
                    shows(numbersShown(g->layers().dynamicList()), kmh(10.0) - kmh(20.0), 0.11), kTimeoutMs);
                ws.setCursor(20 * 1'000'000, qt::CursorSource::Programmatic);
            }

            // ---- M: measurements; hovering a max cell draws its position line.
            pressIn(w, Qt::Key_M);
            QTRY_VERIFY_WITH_TIMEOUT(g->layout().keyColumns.measurements, kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
            QVERIFY(!g->staticInfo().measurements.empty());
            {
                const plot::TdGroupLayout& gl = g->layout().groups.front();
                const plot::RectPx cell = gl.keyGeometry.cellRect(0, 0, plot::KeyCell::Max);
                const double dpr = wide(g->layout().dpr);
                const QPointF p((wide(gl.key.x + cell.x) + 0.5 * wide(cell.w)) / dpr,
                                (wide(gl.key.y + cell.y) + 0.5 * wide(cell.h)) / dpr);
                QCOMPARE(g->hitAt(p).kind, plot::HitKind::KeyMeasurement);
                uitest::sendMouse(g, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
                QVERIFY(g->hoverLineX().has_value());
            }

            // ---- O: the offset axis; ALT+SHIFT+RIGHT x4 nudges the reference overlay by 4 x 0.25 m, one
            // step.
            focusComponent(w, g);
            pressIn(w, Qt::Key_O);
            QVERIFY(ws.state().offsetAxis);
            const int beforeNudge = stack.index();
            for (int i = 0; i < 4; ++i) {
                pressIn(w, Qt::Key_Right, Qt::AltModifier | Qt::ShiftModifier);
            }
            QTRY_VERIFY_WITH_TIMEOUT(w.selection().resolve(zeroLap) != nullptr &&
                                         std::abs(w.selection().resolve(zeroLap)->offsetM - 1.0) < 1e-9,
                                     10000);
            QCOMPARE(stack.index(), beforeNudge + 1);
            QCOMPARE(w.selection().resolve(mainLap)->offsetM, 0.0);

            // ---- Track Editor: Generate from GPS, Accept (default sections), add "T1 entry".
            QVERIFY(w.commands().invoke(QStringLiteral("tools.track_editor")));
            TrackEditor& editor = w.trackEditor();
            QTRY_VERIFY(editor.isVisible());
            QVERIFY(editor.setLap(mainLap));
            editor.setMethod(analysis::TrackMethod::Gps);
            editor.generate();
            QTRY_VERIFY_WITH_TIMEOUT(editor.preview() != nullptr, kTimeoutMs);
            QVERIFY(std::abs(editor.preview()->lengthM - circuit.lengthM) < 0.02 * circuit.lengthM);
            editor.accept();
            QTRY_VERIFY_WITH_TIMEOUT(editor.currentMap() != nullptr && !editor.currentMap()->sections.empty(),
                                     kTimeoutMs);
            QCOMPARE(editor.venue(), kVenue);
            const std::shared_ptr<const analysis::TrackMap> generated = editor.currentMap();
            const analysis::TrackSection* firstCorner = nullptr;
            for (const analysis::TrackSection& s : generated->sections) {
                QVERIFY(s.endM - s.startM >= 10.0); // the default sections obey the 10 m minimum
                if (firstCorner == nullptr && s.category == "corner") {
                    firstCorner = &s;
                }
            }
            QVERIFY(firstCorner != nullptr);
            const double entry = std::max(0.0, firstCorner->startM - 40.0);
            QVERIFY(
                editor.addSection(QStringLiteral("T1 entry"), entry, entry + 50.0, QStringLiteral("corner")));
            bool userSection = false;
            for (const analysis::TrackSection& s : editor.currentMap()->sections) {
                userSection = userSection || (s.name == "T1 entry" && s.userDefined);
            }
            QVERIFY(userSection);
            editor.close();
            w.activateWindow(); // the offscreen platform does not give the activation back by itself
            QVERIFY(QTest::qWaitForWindowActive(&w));
            // The Track Sections groups follow the venue's map (SourceLoader phase 2).
            QTRY_VERIFY_WITH_TIMEOUT(groupOf(w, src, qt::rttest::kSections) != nullptr &&
                                         !groupOf(w, src, qt::rttest::kSections)->ranges.empty(),
                                     kTimeoutMs);

            // ---- The "Reports" worksheet: the Channel Report and the Time Report.
            const int reports = worksheetNamed(w, QStringLiteral("Reports"));
            QVERIFY(reports >= 0);
            w.selectWorksheet(reports);
            auto* channelReport = componentOf<ChannelReport>(w);
            auto* timeReport = componentOf<TimeReport>(w);
            QVERIFY(channelReport != nullptr && timeReport != nullptr);
            qt::ReportModel& cr = channelReport->model();
            QTRY_VERIFY_WITH_TIMEOUT(!cr.busy() && cr.pendingCells() == 0 && cr.rowCount() > 0, kTimeoutMs);
            const QJsonObject crBody = bodyOf(*channelReport);
            QCOMPARE(crBody.value(QStringLiteral("deltaVsOverlay0")).toBool(), true);
            // The script's report: scope Laps (the selected ranges), headings Venue > Driver > Lap,
            // avg/min/max.
            const QJsonObject report = crBody.value(QStringLiteral("report")).toObject();
            QCOMPARE(report.value(QStringLiteral("scope")).toString(), QStringLiteral("selectedRanges"));
            QCOMPARE(report.value(QStringLiteral("headers")).toArray(),
                     (QJsonArray{QStringLiteral("Venue"), QStringLiteral("Driver"),
                                 QStringLiteral("Outings:Laps")}));
            for (const QJsonValue& ch : report.value(QStringLiteral("channels")).toArray()) {
                QCOMPARE(ch.toObject().value(QStringLiteral("stats")).toArray(),
                         (QJsonArray{QStringLiteral("avg"), QStringLiteral("min"), QStringLiteral("max")}));
            }
            const QString tsv = channelReport->toTsv();
            QVERIFY(tsv.contains(QLatin1Char('\t')));
            QVERIFY(tsv.contains(kVenue));
            QVERIFY(tsv.contains(QStringLiteral("Demo Driver")));
            QVERIFY(tsv.contains(QStringLiteral("Corr Speed")));
            const QString headerRow = tsv.section(QLatin1Char('\n'), 0, 0);
            for (const char* stat : {"avg", "min", "max"}) {
                QVERIFY2(tsv.contains(QLatin1String(stat)), stat);
            }
            // Main and Overlay 0 are different laps: the delta pairs them below the lap level (D-203 (c)), so
            // the Main row has delta cells.
            int deltaCells = 0;
            for (int r = 0; r < cr.rowCount(); ++r) {
                for (int c = 1; c < cr.columnCount(); ++c) {
                    const QString heading =
                        cr.headerData(c, Qt::Horizontal).toString() + cr.index(r, 0).data().toString();
                    if (heading.contains(QStringLiteral("vs Overlay 0")) &&
                        !cr.index(r, c).data().toString().isEmpty()) {
                        ++deltaCells;
                    }
                }
            }
            QVERIFY2(deltaCells > 0, qPrintable(headerRow));
            channelReport->copyTsv(); // Export TSV (the whole table, 04 section 12 item 7)
            QCOMPARE(QGuiApplication::clipboard()->text(), tsv);

            qt::ReportModel& tr = timeReport->model();
            QTRY_VERIFY_WITH_TIMEOUT(!tr.busy() && tr.rowCount() > 0 && tr.columnCount() > 2, kTimeoutMs);
            bool eclectic = false;
            bool rolling = false;
            for (int r = 0; r < tr.rowCount(); ++r) {
                const int kind = tr.index(r, 0).data(qt::ReportModel::RowKindRole).toInt();
                eclectic = eclectic || kind == 1;
                rolling = rolling || kind == 2;
            }
            QVERIFY(eclectic && rolling);
            // ALT+3: band 3 (3 %) becomes visible; the 69 s lap's sections are 2.99 % slower than the 67 s
            // lap's (every section takes the same share of a lap), so they take its colour.
            const auto bandCells = [&tr](quint32 rgba) {
                int n = 0;
                for (int r = 0; r < tr.rowCount(); ++r) {
                    for (int c = 0; c < tr.columnCount(); ++c) {
                        const QVariant v = tr.index(r, c).data(qt::ReportModel::BackgroundRgbaRole);
                        n += v.isValid() && v.toUInt() == rgba ? 1 : 0;
                    }
                }
                return n;
            };
            constexpr quint32 kBand3 = 0xe09030ff;
            QCOMPARE(bandCells(kBand3), 0);
            focusComponent(w, timeReport);
            const int beforeBand = stack.index();
            pressIn(w, Qt::Key_3, Qt::AltModifier);
            QCOMPARE(stack.index(), beforeBand + 1);
            QVERIFY(bodyOf(*timeReport)
                        .value(QStringLiteral("bands"))
                        .toArray()
                        .at(3)
                        .toObject()
                        .value(QStringLiteral("visible"))
                        .toBool());
            QTRY_VERIFY_WITH_TIMEOUT(bandCells(kBand3) > 0, kTimeoutMs);
            // The report's "project" time format follows options.lapTimeFormat (D-202): the 67 s lap's time.
            const auto lapTimeShown = [&tr](const QString& text) {
                for (int r = 0; r < tr.rowCount(); ++r) {
                    if (tr.index(r, tr.columnCount() - 1).data().toString() == text) {
                        return true;
                    }
                }
                return false;
            };
            QVERIFY(lapTimeShown(QStringLiteral("1:07.000"))); // m:ss.fff, the default
            {
                project::ProjectFile next = w.session().projectFile();
                QJsonObject options =
                    QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
                options.insert(QStringLiteral("lapTimeFormat"), QStringLiteral("ss.fff"));
                Result<JsonDoc> doc =
                    JsonDoc::parse(QJsonDocument(options).toJson(QJsonDocument::Compact).toStdString());
                QVERIFY(doc.ok());
                next.options = std::move(doc).value();
                stack.push(new qt::SetProjectOptionsCommand(w.session(), std::move(next),
                                                            QStringLiteral("Lap time format")));
            }
            QTRY_VERIFY_WITH_TIMEOUT(lapTimeShown(QStringLiteral("67.000")), kTimeoutMs);
            stack.undo();
            QTRY_VERIFY_WITH_TIMEOUT(lapTimeShown(QStringLiteral("1:07.000")), kTimeoutMs);

            // ---- Back on "Braking": Throttle vs Brake with a polynomial-3 trend; a click picks a point.
            w.selectWorksheet(braking);
            auto* scatter = componentOf<ScatterPlot>(w);
            QVERIFY(scatter != nullptr);
            QCOMPARE(scatter->body().xChannel, std::string("THROTTLE"));
            QVERIFY(!scatter->body().traces.empty());
            QCOMPARE(scatter->body().traces.front().yChannel, std::string("BRAKE"));
            QTRY_VERIFY_WITH_TIMEOUT(scatter->staticUpToDate() && !scatter->equationText().isEmpty(),
                                     kTimeoutMs);
            QVERIFY(scatter->equationText().startsWith(QStringLiteral("y = ")));
            QVERIFY(scatter->equationText().contains(QStringLiteral("x^3")));
            {
                // A point with the throttle at 80.9 % (u = 0.1) and the brake off. The log's % channels are
                // stored in the base unit (ratio, D-010) and the scatter shows the base unit without a
                // channel override, so the point's x is the throttle converted to the X channel's display
                // unit.
                const plot::ResolvedChannel& xc = scatter->inputs().slots.front().channels.front();
                QCOMPARE(QString::fromStdString(xc.name), QStringLiteral("THROTTLE"));
                const double target = xc.toDisplay(throttleAt(0.1) / 100.0);
                // The axes come from the channels' SummaryPyramids (the loader's background summary pass).
                QTRY_VERIFY_WITH_TIMEOUT(!scatter->axes().empty() && scatter->axes().front().xMax >= target,
                                         kTimeoutMs);
                const std::vector<plot::ScatterAxes> axes = scatter->axes();
                QVERIFY(!axes.empty());
                const plot::ScatterAxes& a = axes.front();
                const double dpr = static_cast<double>(scatter->inputs().layout.dpr);
                const QPointF p((static_cast<double>(a.plot.x) +
                                 (target - a.xMin) / (a.xMax - a.xMin) * static_cast<double>(a.plot.w)) /
                                    dpr,
                                (static_cast<double>(a.plot.y) +
                                 (a.yMax - 0.0) / (a.yMax - a.yMin) * static_cast<double>(a.plot.h)) /
                                        dpr -
                                    2.0); // 2 px above the plot's bottom edge (brake 0), inside the 8 px pick
                QCOMPARE(scatter->hitAt(p).kind, plot::HitKind::Panel);
                ws.setCursor(0, qt::CursorSource::Programmatic);
                focusComponent(w, scatter);
                uitest::sendMouse(scatter, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
                uitest::sendMouse(scatter, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
                QVERIFY(ws.state().cursorT != 0);
                const double u = static_cast<double>(ws.state().cursorT) / (kLapSeconds[4] * 1e6);
                QVERIFY2(std::abs(xc.toDisplay(throttleAt(u) / 100.0) - target) < 0.04 * xc.toDisplay(1.0),
                         qPrintable(QStringLiteral("the cursor's throttle %1, the point's %2")
                                        .arg(throttleAt(u))
                                        .arg(target)));
            }

            // ---- A: animation at x10, Cursor-only repaints; A again stops.
            g = uitest::graphOf(w);
            QVERIFY(g != nullptr);
            focusComponent(w, g);
            QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
            AnimationController& animation = w.animation();
            // x10 from the Data > Animation Rate menu, as the script's user would choose it (D-203 (g)).
            QAction* x10 = nullptr;
            for (QAction* a : w.findChildren<QAction*>()) {
                if (a->text() == QStringLiteral("x10")) {
                    x10 = a;
                }
            }
            QVERIFY(x10 != nullptr);
            x10->trigger();
            QCOMPARE(animation.rate(), 10.0);
            QCOMPARE(ws.animationRate(), 10.0); // the view model runs the rate the menu shows
            const Micros startT = ws.state().cursorT;
            const int submitted = g->staticBuildsSubmitted();
            QElapsedTimer wall;
            wall.start();
            pressIn(w, Qt::Key_A);
            QVERIFY(animation.animating());
            QTRY_VERIFY_WITH_TIMEOUT(ws.state().cursorT > startT + 8 * 1'000'000, 20000);
            // The cursor advances by the rate times the wall-clock time: at x10, 8 s of log within well under
            // 1.6 s even under load (x4 would need 2 s).
            const double effective = static_cast<double>(ws.state().cursorT - startT) / 1e3 /
                                     static_cast<double>(std::max<qint64>(1, wall.elapsed()));
            QVERIFY2(effective > 5.0, qPrintable(QStringLiteral("effective rate x%1").arg(effective)));
            QVERIFY2(effective < 10.5, qPrintable(QStringLiteral("effective rate x%1").arg(effective)));
            QCOMPARE(g->staticBuildsSubmitted(), submitted);
            pressIn(w, Qt::Key_A);
            QVERIFY(!animation.animating());

            // ---- CTRL+SHIFT+P, "single colour", ENTER: Single Colour Main toggles.
            QVERIFY(!w.selection().snapshot().singleColourMain);
            focusComponent(w, g);
            pressIn(w, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            CommandPalette& palette = w.commandPalette();
            QTRY_VERIFY(palette.isVisible());
            QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("single colour"));
            QVERIFY(!palette.visibleIds().isEmpty());
            QCOMPARE(palette.visibleIds().front(), QStringLiteral("data.single_colour_main"));
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
            QTRY_VERIFY(w.selection().snapshot().singleColourMain);
            QTRY_VERIFY(!palette.isVisible());
            qInfo("demo of 07 section 6.5 complete: %d undo steps", stack.count());
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstDemoM3)
#include "tst_demo_m3.moc"
