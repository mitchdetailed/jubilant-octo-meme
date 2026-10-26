// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M4-01 (07 sections 2.2 and 7.2; D-214): the "Saturday" walkthrough of 0.1, items 1-10 of 07 section 2.2,
// one test function per item named after it, offscreen in the real shell (MainWindow on a private
// configuration directory with the shipped data: the default circuit workbook, the light and dark schemes,
// data/keymaps/default.json), driven through command ids and keys as a user drives it (06 section 22, M4: the
// scripted walkthrough drives the shell through command ids). The data are written by the test
// (saturday/saturday_log.hpp): the day's .ld + .ldx on the AC-M3-04 synthetic circuit, a second logger's
// legacy CSV export and a generic CSV with an unknown unit; every expected value comes from their formulas
// (D-041). Behaviours that M1-M3 tests already cover in depth are exercised here once more end to end through
// the shell; the depth stays with those tests (the item's line in docs/milestones/M4.md section 1 names
// them).
//   item01  open .ld (+ .ldx) and both CSV dialects; laps with trust flags; the Import Log; the Mapping panel
//   item02  project folder, journal autosave, backup/, reopen with the previous selection, Options, the two
//           shipped schemes (complete, legible, switched live), the default circuit workbook
//   item03  the Data window: Main / Overlay 0 / Overlays, the lap-number filter, lap time, delta to fastest,
//           offset, editable trust; N/P, Q, F4, SHIFT+F4, CTRL+F, CTRL+ALT+F, J, W, DELETE
//   item04  workbooks and worksheets with their hotkeys, free placement on the 1 % grid, the Layout Editor's
//           basics, the zoom summary bar's SHIFT+click
//   item05  the ten MVP components of the shipped workbook over the day's laps, time_distance's datum,
//           measurements and variance, animation (A)
//   item06  Channels window search, Channel Editor overrides, aliases and mappings, Channel Status with its
//           resolution trace, the Values window, the Track Map dock
//   item07  maths: a global module with a constant, an enumeration, filter and 2D table items (imported in
//           the Maths Editor), live diagnostics, Pending to Ok, local and transient maths from the channel
//   item08  corrected speed and distance, lap stretching and its per-lap report, Lap Editor, Details editor,
//           Track Editor (both generation methods, default and user sections)
//   item09  the hotkey table: every row of data/keymaps/default.json registered and bound in its context,
//           every command of 06 section 14.4 present; the command palette
//   item10  the raster backend on 20 traces (Static on zoom, Dynamic only on a cursor move) and HiDPI: the
//           same graph re-rendered at DPR 2 and back at DPR 1 (the budgets themselves: `bench`)
//   demo75_offscreenWithoutTheInstaller  07 section 7.5 without the installer: -w and -l, the budget
//           warning and Continue, the Performance field, Help > About, the portable layout
// Every runtime marks the GUI thread NoBlock as esses does; no ESSES_ASSERT may fail on any thread.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "../../libs/widgets/tests/m4_hidpi/dpr_test_support.hpp"
#include "saturday/saturday_log.hpp"
#include "saturday/saturday_shell.hpp"
#include "ui_test_support.hpp"

#include <esses/analysis/report_engine.hpp>
#include <esses/analysis/time_report_engine.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/project/journal.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/components/bar_gauge.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/histogram.hpp>
#include <esses/widgets/components/numeric_gauge.hpp>
#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/components/scatter_plot.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/components/values_list.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/editors_data/channel_editor.hpp>
#include <esses/widgets/editors_data/details_editor.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/editors_data/mapping_panel.hpp>
#include <esses/widgets/editors_maths/active_channel_commands.hpp>
#include <esses/widgets/editors_maths/channel_documents_dialogs.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>
#include <esses/widgets/properties_shell/shell_dialogs.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/about_shell.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <vector>

// The qtmodel synthetic-source helpers (.ld writer, shipped units), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
using namespace esses::saturday;

namespace {

constexpr Micros kSec = 1'000'000;
// The cursor of the value checks: an eighth into the Main (68 s) lap, u = 0.125.
constexpr double kCursorU = 0.125;

QJsonObject jsonFile(const std::filesystem::path& p) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(readAll(p))).object();
}

QJsonObject bodyOf(const ComponentWidget& c) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(c.spec().body.compact())).object();
}

double fastLapTolerance(const qt::ProjectSession& s) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(s.projectFile().options.compact()))
        .object()
        .value(QStringLiteral("fastLapTolerancePct"))
        .toDouble();
}

// The displayed row of a channel in a ValuesModel (by its name cell).
int valuesRow(const qt::ValuesModel& m, const QString& channel) {
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.cellText(r, qt::ValuesModel::NameCol) == channel) {
            return r;
        }
    }
    return -1;
}

std::optional<double> valuesMain(const qt::ValuesModel& m, const QString& channel) {
    const int r = valuesRow(m, channel);
    return r < 0 ? std::nullopt : uitest::numberOf(m.cellText(r, qt::ValuesModel::MainCol));
}

// The base-unit value of a maths channel of `src` at absolute time t, once it is evaluated (nullopt while
// pending).
std::optional<double> mathsValueAt(MainWindow& w, const model::SourceId& src, const std::string& name,
                                   Micros t) {
    const std::optional<maths::ColumnHandle> h =
        w.maths().require(maths::ChannelKey::make(src, model::kNormalLayer, name), Priority::Visible);
    if (!h) {
        return std::nullopt;
    }
    return valueAt(h->series, t);
}

QString mathsStatusText(MainWindow& w, const model::SourceId& src, const std::string& name) {
    const maths::MathStatus s = w.maths().status(maths::ChannelKey::make(src, model::kNormalLayer, name));
    return QStringLiteral("state %1: %2")
        .arg(static_cast<int>(s.state))
        .arg(QString::fromStdString(s.summary));
}

bool mathsOk(MainWindow& w, const model::SourceId& src, const std::string& name) {
    const maths::MathStatus s = w.maths().status(maths::ChannelKey::make(src, model::kNormalLayer, name));
    return s.state == maths::MathState::Ok || s.state == maths::MathState::Warning;
}

// The 06 section 14.4 command ids (every row of the complete default keymap, mouse rows included).
const QStringList kKeymapCommands =
    QStringLiteral(
        "view.data_window view.channels_window view.values_window view.return_focus view.select_worksheet "
        "layout.editor view.worksheet_n view.workbook_n view.prev_worksheet view.next_worksheet "
        "view.prev_workbook view.next_workbook view.channel_status tools.channel_status "
        "view.messages_window view.track_map_window view.command_palette view.reset_docks view.fullscreen "
        "file.open_log file.close_all file.reload_source file.save_project file.print help.contents "
        "file.unload_device file.import_usb edit.undo edit.redo file.new_project file.open_project "
        "file.close_project file.import_data file.export_data component.cut component.copy "
        "component.paste component.delete component.select_all data.toggle_domain data.overlays_toggle "
        "data.overlays_clear data.single_colour_main data.next_range data.prev_range data.select_fastest "
        "data.select_all_fast data.swap_main_overlay0 data.animate data.lap_stretch_toggle "
        "data.filter_all_sources data.select_main data.toggle_overlay0 data.toggle_overlay "
        "data.close_file tools.details tools.channel_editor tools.maths_editor tools.setup_sheets "
        "tools.lap_editor tools.track_editor tools.corrected_speed tools.aliases tools.mappings "
        "tools.units_list tools.options channel.next_active channel.prev_active channel.hide "
        "channel.remove channel.edit_maths channel.status channel.filter channel.scale_offset "
        "channel.zero_at_cursor channel.properties component.properties component.focus_next "
        "component.focus_prev component.legend component.colour_legend component.cycle_layout "
        "component.cycle_style component.colour_band component.maximise component.send_to_background "
        "component.bring_to_front component.export_data component.print_clipboard graph.status_errors "
        "graph.device_events graph.variance graph.measurements graph.copy_cursor_values "
        "cursor.add_annotation zoom.in zoom.out zoom.full zoom.default_range zoom.datum_main zoom.v_in "
        "zoom.v_out zoom.v_full zoom.rubber_band zoom.full_axis zoom.wheel zoom.extend zoom.outing_rect "
        "pan.left pan.right pan.up pan.down pan.back pan.forward pan.centre_cursor pan.drag_axis "
        "cursor.left cursor.right cursor.left_fast cursor.right_fast cursor.home cursor.end cursor.style "
        "cursor.insert_marker cursor.place datum.toggle datum.place datum.swap offset.axis_toggle "
        "offset.main_left offset.main_right offset.main_down offset.main_up offset.overlay_left "
        "offset.overlay_right offset.overlay_down offset.overlay_up offset.drag track.zoom_in "
        "track.zoom_out track.fit")
        .split(QLatin1Char(' '));

// The 06 section 16 colour roles (36) and font roles (16) of scheme-1.
const QStringList kSchemeRoles =
    QStringLiteral(
        "windowBackground cptFrame cptBackground cptForeground cptForeground2 cptFocusBorder "
        "cptCaptionBackground cptCaptionForeground dataBackground dataForeground dataForeground2 dataGrid "
        "dataHeadingBackground dataHeadingForeground dataHighlightBackground dataHighlightForeground "
        "dataError dataUnknown keyBackground cursorMain cursorDatum cursorZoom rangeMark sectionCorner "
        "sectionStraight markerBand annotationMark deviceEvent varianceZero statusOk statusWarn "
        "statusError pendingHatch mainSingleColour overlay0 trackLine")
        .split(QLatin1Char(' '));
const QStringList kSchemeFonts =
    QStringLiteral(
        "axis axisRotated caption key keyStrikeout small colourBands trackLabel equation reportTable "
        "reportHeading dataHeader dataRange valuesHeader valuesItem gauge")
        .split(QLatin1Char(' '));

// The global maths module of item 7 (maths-1; our own): a constant, an enumeration, an expression over both,
// an enumerated expression, a 6-point filter shadowing the logged G Force Long and a 2D table over GEAR.
constexpr std::string_view kSaturdayModule = R"json({
  "$schema": "urn:esses:maths-1",
  "schemaVersion": 1,
  "id": "saturday",
  "enabled": true,
  "constants": [{"name": "Pedal Gain", "value": {"unit": "", "v": 2}}],
  "enumerations": [{"name": "Pedal State",
                    "entries": [{"name": "Coasting", "value": 0}, {"name": "On Throttle", "value": 1},
                                {"name": "Braking", "value": 2}]}],
  "items": [
    {"kind": "expression", "name": "Throttle Doubled", "expr": "'THROTTLE'[%] * 'Pedal Gain'",
     "quantity": "Unitless", "resultUnit": "", "rate": "auto"},
    {"kind": "expression", "name": "Pedal State", "enumeration": "Pedal State", "interp": "Previous",
     "expr": "choose('THROTTLE'[%] > 1, 1, choose('BRAKE'[%] > 1, 2, 0))",
     "quantity": "Unitless", "resultUnit": "", "rate": "auto"},
    {"kind": "filter", "name": "G Force Long", "window": {"points": 6}},
    {"kind": "table2d", "name": "Gear Factor", "interpolation": "linear", "outOfRange": "clamp",
     "axis": {"breakpoints": [1, 2, 3, 4, 5, 6], "expr": "'GEAR'", "unit": ""},
     "values": {"data": [10, 20, 30, 40, 50, 60], "unit": ""},
     "quantity": "Unitless", "resultUnit": ""}
  ]
})json";

} // namespace

class TstWalkthroughSaturday : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        uitest::prepareFontEnvironment();
        // Four screens of DPR 1, 1.25, 1.5 and 2 (the HiDPI sweep's configuration, D-209); the windows open
        // on the first (DPR 1); item 10 moves one to DPR 2 and back.
        dprtest::useSweepScreens("tst_walkthrough_saturday", std::filesystem::path(ESSES_TEST_BINARY_DIR));
    }

private Q_SLOTS:
    void initTestCase() {
        uitest::registerGoldenFonts();
        registerBuiltinComponents();
        QVERIFY(master_.isValid() && extra_.isValid());
        circuit_ = readCircuit();
        QVERIFY(circuit_.lengthM > 2900.0 && circuit_.centre.size() > 2900);
        // The day's logs, written once (a runtime only for the writer's columns); each item opens a copy.
        auto rt = uitest::makeRuntime(4, false);
        {
            const ThreadRole::Scope blocking(ThreadRole::Normal); // writing reads every chunk
            const std::filesystem::path ld =
                qt::test::writeLd(*daySource(circuit_, rt->cache()), fsPath(master_.path()), "saturday");
            QVERIFY(std::filesystem::exists(ld));
            QVERIFY(std::filesystem::exists(fsPath(master_.path()) / "saturday.ldx"));
            legacyCsv_ = writeLegacyCsv(fsPath(extra_.path()), rt->cache());
            unknownCsv_ = writeUnknownUnitsCsv(fsPath(extra_.path()));
        }
        rt->shutdown();
    }

    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    // ---- 1: several .ld (+ .ldx merged) and CSV files; the Import Log; the inline Mapping panel
    // -------------
    void item01_openLdLdxAndCsvImportLogMappingPanel() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        MainWindow& w = *d.w;
        MappingPanel& panel = w.mappingPanel();
        QVERIFY(!panel.isVisible());
        const int messagesBefore = w.messages().rowCount();
        // esses -l saturday.ld -l second-logger.csv -l boost-logger.csv
        w.openLog(d.ld);
        w.openLog(legacyCsv_);
        w.openLog(unknownCsv_);
        QTRY_VERIFY_WITH_TIMEOUT(panel.blocksPublish(), kTimeoutMs);
        QVERIFY(panel.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(w, "saturday.ld") != nullptr &&
                                     sourceNamed(w, "second-logger.csv") != nullptr,
                                 kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(d.quiet(), kTimeoutMs);
        QVERIFY(sourceNamed(w, "boost-logger.csv") == nullptr); // waits for the panel

        // The .ld with its .ldx: the beacons of the companion make the laps, each with its class and trust.
        const auto day = sourceNamed(w, "saturday.ld");
        d.src = day->id;
        QTRY_COMPARE_WITH_TIMEOUT(w.distance().state(d.src), qt::DistanceState::Ready, kTimeoutMs);
        const std::vector<model::RangeRef> laps = w.selection().displayedRanges(d.src);
        QCOMPARE(laps.size(), kLapSeconds.size());
        for (std::size_t k = 0; k < laps.size(); ++k) {
            const model::Range* r = w.selection().resolve(laps[k]);
            QVERIFY(r != nullptr);
            QCOMPARE(r->end - r->start, static_cast<Micros>(std::llround(kLapSeconds[k] * 1e6)));
            const model::LapClass want = k == 0                 ? model::LapClass::Out
                                         : k + 1 == laps.size() ? model::LapClass::In
                                                                : model::LapClass::Flying;
            QCOMPARE(r->lapClass, want);
            QCOMPARE(r->trusted, want == model::LapClass::Flying); // out and in laps are not trusted
        }
        bool venue = false;
        for (const model::Detail& detail : day->details) {
            venue = venue || (detail.id == "Venue" &&
                              DetailsEditor::valueText(detail.value) == QString::fromStdString(kVenue));
        }
        QVERIFY(venue); // the log's details came with it

        // The legacy CSV (a second logger): its channels at 20 Hz with the formula's values.
        const auto csv = sourceNamed(w, "second-logger.csv");
        QCOMPARE(csv->prov.importerId, std::string("motec.csv"));
        const model::Channel* oil = channelOf(*csv, "Oil Pressure");
        const model::Channel* water = channelOf(*csv, "Water Temp");
        QVERIFY(oil != nullptr && water != nullptr);
        QVERIFY(oil->desc.rate == (Rate{kCsvRateHz, 1}));
        const auto oilAt5 = valueAt(oil->series, 5 * kSec);
        const auto waterAt5 = valueAt(water->series, 5 * kSec);
        QVERIFY(oilAt5 && waterAt5);
        QVERIFY2(std::abs(*oilAt5 - oilKpa(5.0) * 1000.0) < 0.06 * 1000.0,
                 qPrintable(QString::number(*oilAt5)));
        QVERIFY2(std::abs(*waterAt5 - (waterC(5.0) + 273.15)) < 0.06, qPrintable(QString::number(*waterAt5)));

        // The Import Log: rows of origin Import for the files opened.
        QSet<QString> logged;
        for (int r = messagesBefore; r < w.messages().rowCount(); ++r) {
            const qt::Message& m = w.messages().at(r);
            if (m.origin == qt::Message::Origin::Import) {
                logged.insert(QFileInfo(m.source).fileName());
            }
        }
        QVERIFY2(logged.contains(QStringLiteral("second-logger.csv")),
                 qPrintable(QStringList(logged.values()).join(',')));

        // The Mapping panel: one row (Boost, "zorks"); Resolve refuses it incomplete, then publishes the
        // file.
        QCOMPARE(panel.rows().size(), std::size_t{1});
        QCOMPARE(panel.rows()[0].loggedName, std::string("Boost"));
        QVERIFY(!panel.resolve());
        QVERIFY(panel.setUnit(0, QStringLiteral("kPa")));
        QVERIFY(panel.resolve());
        QTRY_VERIFY_WITH_TIMEOUT(sourceNamed(w, "boost-logger.csv") != nullptr, kTimeoutMs);
        QTRY_VERIFY(!panel.isVisible());
        const auto boost = sourceNamed(w, "boost-logger.csv");
        const model::Channel* b = channelOf(*boost, "Boost");
        QVERIFY(b != nullptr);
        QCOMPARE(qt::test::shippedUnits()->quantity(b->desc.quantity).name, std::string("Pressure"));
        // Boost at sample 4 (t = 2 s) is 100 + 5 x 4 = 120 kPa.
        const auto boostAt2 = valueAt(b->series, 2 * kSec);
        QVERIFY(boostAt2 && std::abs(*boostAt2 - 120'000.0) < 1.0);
        QCOMPARE(w.session().undoStack().undoText(), QStringLiteral("Resolve mapping"));
        QCOMPARE(w.selection().sources().size(), std::size_t{3});
    }

    // ---- 2: project folder, journal, backup/, reopen, Options, the shipped schemes and workbook
    // -------------
    void item02_projectJournalBackupReopenOptionsSchemesWorkbook() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        // The default circuit workbook (D-202 (a); 07 section 6.1 "Data").
        QCOMPARE(worksheetNames(*d.w),
                 (QStringList{"Overlay", "Driver", "Overview", "Braking", "Corners", "Reports", "Track"}));
        QCOMPARE(d.w->activeWorksheetIndex(), 0);
        // Every component sits on its worksheet's grid (06 section 4.3 item 2: moves snap to it) and is at
        // least 4 x 4 cells; every component type is one the shell builds.
        for (const project::WorksheetDef& sheet : d.w->session().workbooks().front().worksheets) {
            for (const project::ComponentSpec& c : sheet.components) {
                const double gx = sheet.grid[0];
                const double gy = sheet.grid[1];
                const QString at = QString::fromStdString(sheet.name + " " + c.type);
                for (const auto& [v, g] : {std::pair{c.rect.l, gx}, std::pair{c.rect.r, gx},
                                           std::pair{c.rect.t, gy}, std::pair{c.rect.b, gy}}) {
                    QVERIFY2(std::abs(v * g - std::round(v * g)) < 1e-9,
                             qPrintable(at + QStringLiteral(" off the grid")));
                }
                QVERIFY2((c.rect.r - c.rect.l) * gx >= 4.0 - 1e-9 && (c.rect.b - c.rect.t) * gy >= 4.0 - 1e-9,
                         qPrintable(at));
                QVERIFY2(ComponentWidget::isRegistered(QString::fromStdString(c.type)), qPrintable(at));
            }
        }
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        d.w->selectWorksheet(3);
        const model::RangeRef main = d.laps[kFastest];
        const model::RangeRef reference = d.laps[3];
        QVERIFY(d.w->close()); // the log becomes the project's previous data; view state written
        QVERIFY(uitest::showAndActivate(*d.w));
        const std::string onDisk = readAll(d.folder / "project.json");
        QVERIFY(!onDisk.empty());

        // Options through the dialog: one document edit.
        QUndoStack& stack = d.w->session().undoStack();
        const int steps = stack.count();
        d.w->actions().action(QStringLiteral("tools.options"))->trigger();
        OptionsDialog& options = d.w->optionsDialog();
        QTRY_VERIFY(options.isVisible());
        auto* tolerance = options.findChild<QDoubleSpinBox*>(QStringLiteral("fastLapTolerancePct"));
        QVERIFY(tolerance != nullptr);
        tolerance->setValue(2.5);
        options.accept();
        QCOMPARE(stack.count(), steps + 1);
        QCOMPARE(fastLapTolerance(d.w->session()), 2.5);
        QVERIFY(d.w->session().isDirty());

        // The journal: the autosave writes .journal/, never the documents.
        d.w->session().setAutosaveInterval(std::chrono::milliseconds(50));
        QTRY_VERIFY_WITH_TIMEOUT(filesIn(d.folder / ".journal") >= 1, kTimeoutMs);
        QCOMPARE(readAll(d.folder / "project.json"), onDisk);

        // CTRL+S: the documents written, the previous project.json copied to backup/, the journal cleared.
        const std::size_t backups = filesIn(d.folder / "backup", "project.");
        d.w->returnFocus();
        key(*d.w, QStringLiteral("Ctrl+S"));
        QVERIFY(!d.w->session().isDirty());
        QCOMPARE(filesIn(d.folder / ".journal"), std::size_t{0});
        QCOMPARE(filesIn(d.folder / "backup", "project."), backups + 1);
        bool backedUp = false;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(d.folder / "backup", ec)) {
            backedUp = backedUp || readAll(e.path()) == onDisk;
        }
        QVERIFY(backedUp);
        QCOMPARE(jsonFile(d.folder / "project.json")
                     .value(QStringLiteral("options"))
                     .toObject()
                     .value(QStringLiteral("fastLapTolerancePct"))
                     .toDouble(),
                 2.5);

        // Reopen in a new window: the previous data, the selection and the worksheet come back.
        QVERIFY(d.w->close());
        d.w = std::make_unique<MainWindow>(*d.rt, uitest::pathsFor(d.config.path()));
        d.w->setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
        QVERIFY(d.w->openProject(d.folder));
        QVERIFY(uitest::showAndActivate(*d.w));
        QTRY_VERIFY_WITH_TIMEOUT(!d.w->sessionRestore().running() && d.w->selection().hasMain(), kTimeoutMs);
        QVERIFY(d.w->selection().snapshot().main == main);
        QVERIFY(d.w->selection().snapshot().overlay0 == std::optional<model::RangeRef>(reference));
        QCOMPARE(d.w->activeWorksheetIndex(), 3);
        QCOMPARE(fastLapTolerance(d.w->session()), 2.5);
        QVERIFY(d.w->recoveryDialog() == nullptr);

        // The two shipped schemes: every role and font of 06 section 16, the palettes' sizes, and legible
        // pairs (06 section 20 item 3, section 13.9 item 4: at least 3:1).
        for (const char* file : {"light", "dark"}) {
            const QJsonObject s =
                jsonFile(qt::test::repoRoot() / "data" / "schemes" / (std::string(file) + ".scheme.json"));
            const QJsonObject roles = s.value(QStringLiteral("roles")).toObject();
            const QJsonObject fonts = s.value(QStringLiteral("fonts")).toObject();
            const QStringList roleNames = roles.keys();
            const QStringList fontNames = fonts.keys();
            QCOMPARE(QSet<QString>(roleNames.begin(), roleNames.end()),
                     QSet<QString>(kSchemeRoles.begin(), kSchemeRoles.end()));
            QCOMPARE(QSet<QString>(fontNames.begin(), fontNames.end()),
                     QSet<QString>(kSchemeFonts.begin(), kSchemeFonts.end()));
            const QJsonArray channels = s.value(QStringLiteral("channelPalette")).toArray();
            const QJsonArray overlays = s.value(QStringLiteral("overlayPalette")).toArray();
            QVERIFY(channels.size() >= 10);
            QCOMPARE(overlays.size(), 9);
            const auto role = [&roles](const char* r) { return roles.value(QLatin1String(r)).toString(); };
            for (const auto& [fg, bg] : std::vector<std::pair<const char*, const char*>>{
                     {"cptFocusBorder", "cptBackground"},
                     {"cptForeground", "cptBackground"},
                     {"cptForeground2", "cptBackground"},
                     {"cptCaptionForeground", "cptCaptionBackground"},
                     {"dataForeground", "dataBackground"},
                     {"dataForeground2", "dataBackground"},
                     {"dataHeadingForeground", "dataHeadingBackground"},
                     {"dataHighlightForeground", "dataHighlightBackground"},
                     {"dataError", "dataBackground"},
                     {"dataUnknown", "dataBackground"},
                     {"cursorMain", "cptBackground"},
                     {"cursorDatum", "cptBackground"},
                     {"overlay0", "cptBackground"},
                     {"mainSingleColour", "cptBackground"},
                     {"statusOk", "cptBackground"},
                     {"statusWarn", "cptBackground"},
                     {"statusError", "cptBackground"},
                     {"trackLine", "cptBackground"},
                     {"cptForeground", "keyBackground"}}) {
                const double ratio = contrast(role(fg), role(bg));
                QVERIFY2(ratio >= 3.0,
                         qPrintable(QStringLiteral("%1: %2 on %3 is %4:1")
                                        .arg(QLatin1String(file), QLatin1String(fg), QLatin1String(bg))
                                        .arg(ratio)));
            }
            for (const QJsonArray& palette : {channels, overlays}) {
                for (const QJsonValue& c : palette) {
                    QVERIFY2(contrast(c.toString(), role("cptBackground")) >= 3.0,
                             qPrintable(QLatin1String(file) + QStringLiteral(": ") + c.toString()));
                }
            }
        }
        // Switched live: the application palette follows the scheme's window background.
        SchemeSelector& schemes = d.w->schemeSelector();
        QVERIFY(schemes.schemeNames().contains(QStringLiteral("Esses Light")));
        QVERIFY(schemes.schemeNames().contains(QStringLiteral("Esses Dark")));
        const QString darkWindow = jsonFile(qt::test::repoRoot() / "data" / "schemes" / "dark.scheme.json")
                                       .value(QStringLiteral("roles"))
                                       .toObject()
                                       .value(QStringLiteral("windowBackground"))
                                       .toString();
        const QString lightWindow = jsonFile(qt::test::repoRoot() / "data" / "schemes" / "light.scheme.json")
                                        .value(QStringLiteral("roles"))
                                        .toObject()
                                        .value(QStringLiteral("windowBackground"))
                                        .toString();
        QVERIFY(schemes.selectScheme(QStringLiteral("Esses Dark")));
        QCOMPARE(QString::fromStdString(d.w->session().scheme()->name), QStringLiteral("Esses Dark"));
        QCOMPARE(QApplication::palette().color(QPalette::Window).name(), darkWindow);
        QVERIFY(schemes.selectScheme(QStringLiteral("Esses Light")));
        QCOMPARE(QApplication::palette().color(QPalette::Window).name(), lightWindow);
    }

    // ---- 3: the Data window: slots, filters, columns, trust; its hotkeys ----------------------------------
    void item03_dataWindowFiltersLapNumbersAndHotkeys() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        MainWindow& w = *d.w;
        qt::DataSelectionModel& sel = w.selection();
        const std::vector<model::RangeRef>& laps = d.laps;
        DataWindow& data = w.dataWindow();
        qt::DataWindowModel& model = data.model();

        // F8, then ENTER / CTRL+ENTER / ALT+ENTER on highlighted laps: Main, Overlay 0, Overlays 1 and 2.
        w.returnFocus();
        key(w, QStringLiteral("F8"));
        QCOMPARE(w.docks().focusedDockId(), QStringLiteral("data"));
        data.highlight(laps[1]);
        key(w, QStringLiteral("Return"));
        data.highlight(laps[2]);
        key(w, QStringLiteral("Ctrl+Return"));
        data.highlight(laps[3]);
        key(w, QStringLiteral("Alt+Return"));
        data.highlight(laps[4]);
        key(w, QStringLiteral("Alt+Return"));
        QVERIFY(sel.snapshot().main == laps[1]);
        QVERIFY(sel.snapshot().overlay0 == std::optional<model::RangeRef>(laps[2]));
        QCOMPARE(sel.snapshot().overlays, (std::vector<model::RangeRef>{laps[3], laps[4]}));

        // The lap time (m:ss.fff) and delta to the fastest (68 s) columns.
        const auto cell = [&](const model::RangeRef& r, int column) {
            const QModelIndex i = model.indexOf(r);
            return model.index(i.row(), column, i.parent()).data().toString();
        };
        QCOMPARE(cell(laps[1], qt::DataWindowModel::TimeCol), QStringLiteral("1:10.000"));
        QCOMPARE(cell(laps[2], qt::DataWindowModel::TimeCol), QStringLiteral("1:08.000"));
        QCOMPARE(cell(laps[1], qt::DataWindowModel::DeltaFastestCol), QStringLiteral("+2.000"));
        QCOMPARE(cell(laps[3], qt::DataWindowModel::DeltaFastestCol), QStringLiteral("+0.500"));

        // Hotkeys of the worksheet context from the graph.
        TimeDistanceGraph* g = uitest::graphOf(w);
        QVERIFY(g != nullptr);
        focusComponent(w, g);
        key(w, QStringLiteral("Shift+F4")); // clear Overlay 0 and the overlays
        QVERIFY(!sel.snapshot().overlay0.has_value() && sel.snapshot().overlays.empty());
        key(w, QStringLiteral("N")); // Main to the next displayed lap
        QVERIFY(sel.snapshot().main == laps[2]);
        key(w, QStringLiteral("P"));
        QVERIFY(sel.snapshot().main == laps[1]);
        sel.toggleOverlay0(laps[3]);
        key(w, QStringLiteral("Q")); // swap Main and Overlay 0
        QVERIFY(sel.snapshot().main == laps[3]);
        QVERIFY(sel.snapshot().overlay0 == std::optional<model::RangeRef>(laps[1]));
        sel.toggleOverlay(laps[4]);
        key(w, QStringLiteral("F4")); // hide the overlays without deselecting them
        QVERIFY(sel.snapshot().overlaysHidden);
        QCOMPARE(sel.snapshot().overlays.size(), std::size_t{1});
        key(w, QStringLiteral("F4"));
        QVERIFY(!sel.snapshot().overlaysHidden);
        key(w, QStringLiteral("J")); // single-colour Main
        QVERIFY(sel.snapshot().singleColourMain);
        key(w, QStringLiteral("J"));
        QVERIFY(!sel.snapshot().singleColourMain);
        key(w, QStringLiteral("Shift+F4"));
        key(w, QStringLiteral("Ctrl+F")); // the fastest trusted flying lap: 68 s
        QVERIFY(sel.snapshot().main == laps[kFastest]);
        key(w, QStringLiteral("Ctrl+Alt+F")); // and every lap within 1 %: 68.5 s (70 s is 2.9 % slower)
        QVERIFY(sel.snapshot().main == laps[kFastest]);
        QCOMPARE(sel.snapshot().overlays, std::vector<model::RangeRef>{laps[3]});
        key(w, QStringLiteral("W")); // zoom to the Main lap
        QTRY_VERIFY_WITH_TIMEOUT(w.activeWorksheet()->state().zoom.a == 0.0 &&
                                     std::abs(w.activeWorksheet()->state().zoom.b - 68.0 * kSec) < 1.0,
                                 kTimeoutMs);

        // Editable trust: the 68 s lap untrusted, CTRL+F picks 68.5 s; undo trusts it again.
        QUndoStack& stack = w.session().undoStack();
        const int before = stack.count();
        const QModelIndex fastest = model.indexOf(laps[kFastest]);
        QVERIFY(model.setData(model.index(fastest.row(), qt::DataWindowModel::TrustedCol, fastest.parent()),
                              Qt::Unchecked, Qt::CheckStateRole));
        QTRY_VERIFY_WITH_TIMEOUT(!sel.resolve(laps[kFastest])->trusted, kTimeoutMs);
        QCOMPARE(stack.count(), before + 1);
        focusComponent(w, g);
        key(w, QStringLiteral("Ctrl+F"));
        QVERIFY(sel.snapshot().main == laps[3]);
        stack.undo();
        QTRY_VERIFY_WITH_TIMEOUT(sel.resolve(laps[kFastest])->trusted, kTimeoutMs);

        // The offset column: O, then CTRL+SHIFT+RIGHT offsets Main by one step; O again zeroes it.
        sel.setMain(laps[kFastest]);
        focusComponent(w, g);
        key(w, QStringLiteral("O"));
        QVERIFY(w.activeWorksheet()->state().offsetAxis);
        key(w, QStringLiteral("Ctrl+Shift+Right"));
        QTRY_VERIFY_WITH_TIMEOUT(!cell(laps[kFastest], qt::DataWindowModel::OffsetCol).isEmpty(), kTimeoutMs);
        focusComponent(w, g);
        key(w, QStringLiteral("O"));
        QTRY_VERIFY_WITH_TIMEOUT(cell(laps[kFastest], qt::DataWindowModel::OffsetCol).isEmpty(), kTimeoutMs);

        // The lap-number filter: "1,3" shows the flying laps numbered 1 and 3 (and Main, which holds a slot);
        // a malformed list is refused with its reason.
        LapFilterEditor* editor = data.editLapNumbers(d.src);
        QVERIFY(editor != nullptr);
        editor->text().setText(QStringLiteral("1-"));
        QVERIFY(!editor->apply());
        QVERIFY(!editor->error().text().isEmpty());
        editor->text().setText(QStringLiteral("1,3"));
        QVERIFY(editor->apply());
        QCOMPARE(sel.filter(d.src).kind, qt::FilterSpec::Kind::LapNumbers);
        std::set<int> shown;
        for (const model::RangeRef& r : sel.displayedRanges(d.src)) {
            shown.insert(sel.resolve(r)->number);
        }
        const int mainNumber = sel.resolve(laps[kFastest])->number;
        QVERIFY(shown.count(1) == 1 && shown.count(3) == 1 && shown.count(mainNumber) == 1);
        QVERIFY(shown.size() <= 3);
        qt::FilterSpec all;
        all.kind = qt::FilterSpec::Kind::All;
        sel.setFilter(d.src, all);
        QCOMPARE(sel.displayedRanges(d.src).size(), kLapSeconds.size());

        // DELETE on the source's row of the Data window closes it.
        w.docks().focus(QStringLiteral("data"));
        data.highlight(laps[1]);
        data.tree().setCurrentIndex(data.tree().currentIndex().parent());
        QVERIFY(data.highlightedIsSource());
        key(w, QStringLiteral("Delete"));
        QTRY_VERIFY_WITH_TIMEOUT(sel.sources().empty(), kTimeoutMs);
    }

    // ---- 4: workbooks and worksheets, free placement, Layout Editor, the zoom summary bar
    // ------------------
    void item04_workbooksWorksheetsFreePlacementLayoutEditorZoomSummary() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        QUndoStack& stack = w.session().undoStack();
        const int mark = stack.index();
        const int sheets = w.worksheetCount();
        QCOMPARE(sheets, 7);

        // Worksheet keys from a component: 3, 7, an absent 8 (no-op), F12 wrapping to 1, F11 back to 7.
        const auto fromGraph = [&w] {
            ComponentWidget* c = w.canvas().components().front();
            focusComponent(w, c);
        };
        fromGraph();
        key(w, QStringLiteral("3"));
        QCOMPARE(w.activeWorksheetIndex(), 2);
        fromGraph();
        key(w, QStringLiteral("7"));
        QCOMPARE(w.activeWorksheetIndex(), 6);
        fromGraph();
        key(w, QStringLiteral("8"));
        QCOMPARE(w.activeWorksheetIndex(), 6);
        key(w, QStringLiteral("F12"));
        QCOMPARE(w.activeWorksheetIndex(), 0);
        key(w, QStringLiteral("F11"));
        QCOMPARE(w.activeWorksheetIndex(), 6);
        // F7: the Select Worksheet dialog.
        key(w, QStringLiteral("F7"));
        QTRY_VERIFY(w.selectWorksheetDialog() != nullptr && w.selectWorksheetDialog()->isVisible());
        w.selectWorksheetDialog()->reject();
        QTRY_VERIFY(w.selectWorksheetDialog() == nullptr);
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // A second workbook; CTRL+2 and CTRL+1 switch between them.
        {
            project::WorkbookFile book = w.session().workbooks().front();
            book.id = {};
            book.name = "Practice";
            stack.push(new qt::AddWorkbookCommand(w.session(), book));
        }
        QCOMPARE(w.workbookSelector().count(), 2);
        w.returnFocus();
        key(w, QStringLiteral("Ctrl+2"));
        QCOMPARE(w.session().activeWorkbook(), 1);
        key(w, QStringLiteral("Ctrl+1"));
        QCOMPARE(w.session().activeWorkbook(), 0);

        // Free placement on the 1 % grid: ALT+LEFT nudges the focused values_list one grid cell, one step.
        w.selectWorksheet(0);
        auto* values = componentOf<ValuesList>(w);
        QVERIFY(values != nullptr);
        QCOMPARE(values->spec().rect.l, 0.75);
        focusComponent(w, values);
        const int beforeNudge = stack.index();
        key(w, QStringLiteral("Alt+Left"));
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(componentOf<ValuesList>(w)->spec().rect.l - 0.74) < 1e-9,
                                 kTimeoutMs);
        QVERIFY(std::abs(componentOf<ValuesList>(w)->spec().rect.r - 0.99) < 1e-9);
        QCOMPARE(stack.index(), beforeNudge + 1);
        stack.undo();
        QTRY_COMPARE(componentOf<ValuesList>(w)->spec().rect.l, 0.75);

        // The Layout Editor: a duplicated worksheet appears as a tab; locking the layout refuses a nudge.
        QVERIFY(w.commands().invoke(QStringLiteral("layout.editor")));
        LayoutEditor& editor = w.layoutEditor();
        QTRY_VERIFY(editor.isVisible());
        editor.setCurrent(0, 0);
        QVERIFY(editor.duplicate());
        QTRY_COMPARE(w.worksheetCount(), sheets + 1);
        QVERIFY(editor.setLocked(true));
        QTRY_VERIFY(w.canvas().layoutLocked());
        w.selectWorksheet(0);
        values = componentOf<ValuesList>(w);
        focusComponent(w, values);
        key(w, QStringLiteral("Alt+Left"));
        QCOMPARE(componentOf<ValuesList>(w)->spec().rect.l, 0.75);
        QVERIFY(editor.setLocked(false));
        QTRY_VERIFY(!w.canvas().layoutLocked());
        editor.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // The zoom summary bar: SHIFT+click 20 s beyond the zoom's end extends it there.
        w.activeWorksheet()->zoomToMainRange();
        w.activeWorksheet()->zoomTo(plot::XSpan{0.0, 20.0 * kSec});
        ZoomSummaryBar& bar = w.zoomSummaryBar();
        QTRY_VERIFY(bar.isVisible() && bar.width() > 100);
        const double target = w.activeWorksheet()->state().zoom.b + 20.0 * kSec;
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::ShiftModifier,
                          QPoint(static_cast<int>(std::lround(bar.pxOf(target))), bar.height() / 2));
        const double pixel = bar.summary().extent.length() / std::max(1, bar.width() - 8);
        QVERIFY(w.activeWorksheet()->state().zoom.a == 0.0);
        QVERIFY2(std::abs(w.activeWorksheet()->state().zoom.b - target) <= 2.0 * pixel,
                 qPrintable(QString::number(w.activeWorksheet()->state().zoom.b)));

        // Every document edit undoes back to the start.
        while (stack.index() > mark) {
            stack.undo();
        }
        QCOMPARE(w.worksheetCount(), sheets);
        QCOMPARE(w.workbookSelector().count(), 1);
    }

    // ---- 5: the ten components of the shipped workbook; datum, measurements, variance; animation
    // -----------
    void item05_theTenComponentsAndAnimation() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        const double L = circuit_.lengthM;
        const double P = kLapSeconds[kFastest];
        const double v = speedMs(L, P, kCursorU); // m/s at the cursor
        const Micros cursor = static_cast<Micros>(std::llround(kCursorU * P * 1e6));
        QSet<QString> seen;
        const auto settled = [](LayeredComponent* c) {
            return c->staticUpToDate() && c->staticBuildsDelivered() > 0;
        };
        const auto show = [&](const char* sheet) {
            const int i = worksheetNamed(w, QLatin1String(sheet));
            QVERIFY(i >= 0);
            w.selectWorksheet(i);
            w.activeWorksheet()->setCursor(cursor, qt::CursorSource::Programmatic);
            for (const QString& t : componentTypes(w)) {
                seen.insert(t);
            }
        };

        // Overlay: time_distance (the key's Corr Speed at the cursor) and values_list (THROTTLE, GEAR).
        show("Overlay");
        TimeDistanceGraph* g = uitest::graphOf(w);
        QVERIFY(g != nullptr);
        QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
        QTRY_VERIFY_WITH_TIMEOUT(shows(numbersShown(g->layers().dynamicList()), 3.6 * v, 0.15), kTimeoutMs);
        auto* list = componentOf<ValuesList>(w);
        QVERIFY(list != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(valuesMain(list->model(), QStringLiteral("THROTTLE")).has_value(),
                                 kTimeoutMs);
        QVERIFY2(std::abs(*valuesMain(list->model(), QStringLiteral("THROTTLE")) - throttlePct(kCursorU)) <
                     0.11,
                 qPrintable(list->model().cellText(valuesRow(list->model(), QStringLiteral("THROTTLE")),
                                                   qt::ValuesModel::MainCol)));
        QTRY_COMPARE_WITH_TIMEOUT(valuesMain(list->model(), QStringLiteral("GEAR")).value_or(-1.0), gearOf(v),
                                  kTimeoutMs);
        // The time_distance completion: D and SPACE place the datum, M the measurements, F3 the variance of
        // Overlay 0.
        focusComponent(w, g);
        key(w, QStringLiteral("D"));
        QVERIFY(w.workspace().datumT().has_value());
        key(w, QStringLiteral("Space"));
        QVERIFY(w.workspace().datumT() == std::optional<Micros>(cursor));
        key(w, QStringLiteral("M"));
        QTRY_VERIFY_WITH_TIMEOUT(g->layout().keyColumns.measurements, kTimeoutMs);
        key(w, QStringLiteral("F3"));
        QTRY_VERIFY_WITH_TIMEOUT(g->layout().variance.h > 0.0F, kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
        QVERIFY(g->staticInfo().varianceTables >= 1);
        QVERIFY(!g->staticInfo().measurements.empty());
        key(w, QStringLiteral("F3"));
        key(w, QStringLiteral("M"));
        // Animation (A): the cursor advances; A again stops it.
        const Micros startT = w.activeWorksheet()->state().cursorT;
        key(w, QStringLiteral("A"));
        QVERIFY(w.animation().animating());
        QTRY_VERIFY_WITH_TIMEOUT(w.activeWorksheet()->state().cursorT > startT + kSec / 2, kTimeoutMs);
        key(w, QStringLiteral("A"));
        QVERIFY(!w.animation().animating());
        w.activeWorksheet()->setCursor(cursor, qt::CursorSource::Programmatic);

        // Overview: the numeric gauges (Corr Speed in km/h, GEAR) and the bar gauges (THROTTLE, BRAKE) at the
        // cursor; the track position over the map generated from GPS when the log opened.
        show("Overview");
        const std::vector<NumericGauge*> numeric = componentsOf<NumericGauge>(w);
        QCOMPARE(numeric.size(), std::size_t{2});
        for (NumericGauge* n : numeric) {
            QTRY_VERIFY_WITH_TIMEOUT(settled(n), kTimeoutMs);
        }
        QTRY_VERIFY_WITH_TIMEOUT(shows(numbersShown(numeric[0]->layers().dynamicList()), 3.6 * v, 0.15),
                                 kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(shows(numbersShown(numeric[1]->layers().dynamicList()), gearOf(v), 1e-9),
                                 kTimeoutMs);
        const std::vector<BarGauge*> bars = componentsOf<BarGauge>(w);
        QCOMPARE(bars.size(), std::size_t{2});
        for (BarGauge* b : bars) {
            QTRY_VERIFY_WITH_TIMEOUT(settled(b), kTimeoutMs);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(
            shows(numbersShown(bars[0]->layers().dynamicList()), throttlePct(kCursorU), 0.6),
            qPrintable(listed(numbersShown(bars[0]->layers().dynamicList()))), kTimeoutMs);
        auto* position = componentOf<TrackPosition>(w);
        QVERIFY(position != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(position->trackMap() != nullptr && position->staticUpToDate(), kTimeoutMs);
        QVERIFY(std::abs(position->trackMap()->lengthM - L) < 0.02 * L);

        // Braking: the brake histogram and the throttle-vs-brake scatter with its trend equation.
        show("Braking");
        auto* histogram = componentOf<Histogram>(w);
        auto* scatter = componentOf<ScatterPlot>(w);
        QVERIFY(histogram != nullptr && scatter != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(settled(histogram), kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            settled(scatter) && scatter->equationText().startsWith(QStringLiteral("y = ")), kTimeoutMs);

        // Corners: the G-G scatter and the track position with section labels.
        show("Corners");
        for (ScatterPlot* s : componentsOf<ScatterPlot>(w)) {
            QTRY_VERIFY_WITH_TIMEOUT(settled(s), kTimeoutMs);
        }
        QTRY_VERIFY_WITH_TIMEOUT(componentOf<TrackPosition>(w)->staticUpToDate(), kTimeoutMs);

        // Reports: the channel report's Main row against the formulas (throttle average 100 / pi %, maximum
        // 100 %; Corr Speed between 0.7 and 1.3 times the lap's mean speed L / P), and the time report's lap
        // times.
        show("Reports");
        auto* channelReport = componentOf<ChannelReport>(w);
        auto* timeReport = componentOf<TimeReport>(w);
        QVERIFY(channelReport != nullptr && timeReport != nullptr);
        qt::ReportModel& cr = channelReport->model();
        QTRY_VERIFY_WITH_TIMEOUT(!cr.busy() && cr.pendingCells() == 0 && cr.channelTable().has_value(),
                                 kTimeoutMs);
        {
            const analysis::ReportTable& t = *cr.channelTable();
            const QJsonArray channels = bodyOf(*channelReport)
                                            .value(QStringLiteral("report"))
                                            .toObject()
                                            .value(QStringLiteral("channels"))
                                            .toArray();
            std::optional<std::size_t> mainRow;
            for (std::size_t r = 0; r < t.rows.size(); ++r) {
                if (t.rows[r].role == analysis::ReportSlotRole::Main) {
                    mainRow = r;
                }
            }
            QVERIFY(mainRow.has_value());
            const auto stat = [&](const QString& channel, analysis::ReportStat s) -> std::optional<double> {
                for (std::size_t c = 0; c < t.columns.size(); ++c) {
                    const QString name = channels.at(static_cast<int>(t.columns[c].channel))
                                             .toObject()
                                             .value(QStringLiteral("name"))
                                             .toString();
                    if (name == channel && t.columns[c].stat == s && !t.columns[c].deltaVsOverlay0 &&
                        t.at(*mainRow, c).state == analysis::ReportCellState::Ok) {
                        return t.at(*mainRow, c).value;
                    }
                }
                return std::nullopt;
            };
            const double mean = 3.6 * L / P;
            QVERIFY(stat(QStringLiteral("THROTTLE"), analysis::ReportStat::Avg).has_value());
            QVERIFY2(std::abs(*stat(QStringLiteral("THROTTLE"), analysis::ReportStat::Avg) -
                              100.0 / std::numbers::pi) < 0.2,
                     qPrintable(cr.toTsv()));
            QVERIFY(std::abs(stat(QStringLiteral("THROTTLE"), analysis::ReportStat::Max).value_or(-1) -
                             100.0) < 0.05);
            QVERIFY2(std::abs(stat(QStringLiteral("Corr Speed"), analysis::ReportStat::Max).value_or(-1) -
                              1.3 * mean) < 0.5,
                     qPrintable(cr.toTsv()));
            QVERIFY2(std::abs(stat(QStringLiteral("Corr Speed"), analysis::ReportStat::Min).value_or(-1) -
                              0.7 * mean) < 0.5,
                     qPrintable(cr.toTsv()));
            QVERIFY2(std::abs(stat(QStringLiteral("Corr Speed"), analysis::ReportStat::Avg).value_or(-1) -
                              mean) < 0.3,
                     qPrintable(cr.toTsv()));
        }
        qt::ReportModel& tr = timeReport->model();
        QTRY_VERIFY_WITH_TIMEOUT(!tr.busy() && tr.timeTable().has_value() && !tr.timeTable()->rows.empty(),
                                 kTimeoutMs);
        {
            std::multiset<Micros> lapTimes;
            for (const analysis::TimeReportRow& row : tr.timeTable()->rows) {
                if (!row.cells.empty() && row.cells.back().time) {
                    lapTimes.insert(*row.cells.back().time);
                }
            }
            QVERIFY2(lapTimes.count(static_cast<Micros>(68.0 * kSec)) == 1, qPrintable(tr.toTsv()));
            QVERIFY2(lapTimes.count(static_cast<Micros>(68.5 * kSec)) == 1, qPrintable(tr.toTsv()));
        }

        // Track: the outing graph, the track position and a graph.
        show("Track");
        auto* outing = componentOf<OutingGraph>(w);
        QVERIFY(outing != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(settled(outing), kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(componentOf<TrackPosition>(w)->staticUpToDate(), kTimeoutMs);

        // The ten MVP component types all appeared (07 section 2.2 item 5).
        QCOMPARE(seen, (QSet<QString>{"time_distance", "values_list", "outing", "scatter", "histogram",
                                      "channel_report", "time_report", "track_position", "gauge_numeric",
                                      "gauge_bar"}));
    }

    // ---- 6: Channels window, Channel Editor, aliases and mappings, Channel Status, Values, Track Map
    // -------
    void item06_channelsEditorStatusOverridesAliasesValuesTrackMap() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        QUndoStack& stack = w.session().undoStack();
        const double v = speedMs(circuit_.lengthM, kLapSeconds[kFastest], kCursorU);
        const Micros cursor = static_cast<Micros>(std::llround(kCursorU * kLapSeconds[kFastest] * 1e6));
        w.activeWorksheet()->setCursor(cursor, qt::CursorSource::Programmatic);

        // C: the Channels window with the caret in its search box; the word-prefix search (06 section 7).
        focusComponent(w, uitest::graphOf(w));
        key(w, QStringLiteral("C"));
        ChannelsWindow& channels = w.channelsWindow();
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(&channels.search()));
        QTest::keyClicks(&channels.search(), QStringLiteral("whe sp"));
        QTRY_COMPARE(channels.visibleChannels(), (QStringList{"Wheel Speed FL", "Wheel Speed FR"}));
        channels.search().clear();
        QTest::keyClicks(&channels.search(), QStringLiteral("g f lo"));
        QTRY_COMPARE(channels.visibleChannels(), QStringList{"G Force Long"});
        channels.search().clear();

        // V: the Values window; the focused graph's rows at the cursor.
        w.returnFocus();
        const bool valuesShown = w.docks().isVisible(QStringLiteral("values"));
        focusComponent(w, uitest::graphOf(w));
        key(w, QStringLiteral("V"));
        QCOMPARE(w.docks().isVisible(QStringLiteral("values")), !valuesShown);
        if (!w.docks().isVisible(QStringLiteral("values"))) {
            key(w, QStringLiteral("V"));
        }
        qt::ValuesModel& values = w.valuesWindow().model();
        QTRY_VERIFY_WITH_TIMEOUT(valuesMain(values, QStringLiteral("SPEED")).has_value(), kTimeoutMs);

        // The Channel Editor (CTRL+E): SPEED overridden to km/h with no decimals, one step each (the
        // overrides reach the editors in 0.1; the components follow them at v1.0: docs/milestones/M4.md
        // section 10).
        key(w, QStringLiteral("Ctrl+E"));
        ChannelEditor& editor = w.channelEditor();
        QTRY_VERIFY(editor.isVisible());
        const int steps = stack.count();
        QVERIFY(editor.setDisplayUnit(QStringLiteral("SPEED"), QStringLiteral("km/h")));
        QVERIFY(editor.setDps(QStringLiteral("SPEED"), 0));
        QCOMPARE(stack.count(), steps + 2);
        const project::ChannelOverrides::Entry* o = w.session().overrides().find("SPEED");
        QVERIFY(o != nullptr && o->displayUnit == std::optional<std::string>("km/h"));
        QVERIFY(!editor.setDisplayUnit(QStringLiteral("SPEED"), QStringLiteral("deg C"))); // another quantity
        QVERIFY(!editor.lastRefusal().isEmpty());
        // The Values window shows SPEED at the cursor (m/s or km/h by its display unit).
        {
            const std::optional<double> speed = valuesMain(values, QStringLiteral("SPEED"));
            const QString unit =
                values.cellText(valuesRow(values, QStringLiteral("SPEED")), qt::ValuesModel::UnitCol);
            QVERIFY(speed.has_value());
            const double want = unit == QStringLiteral("km/h") ? 3.6 * v : v;
            QVERIFY2(std::abs(*speed - want) <= 0.051 * (unit == QStringLiteral("km/h") ? 10.0 : 1.0),
                     qPrintable(QString::number(*speed) + QLatin1Char(' ') + unit));
        }

        // Aliases: "Ground Speed" names SPEED; it resolves (Channels window) and reads as SPEED.
        editor.openAliases();
        AliasesDialog* aliases = editor.aliasesDialog();
        QVERIFY(aliases != nullptr);
        project::Aliases doc = aliases->document();
        model::AliasRow row;
        row.name = "Ground Speed";
        row.candidates.push_back(model::AliasCandidate{"SPEED", true});
        doc.table.rows.push_back(row);
        aliases->setDocument(doc);
        QString error;
        QVERIFY2(aliases->apply(&error), qPrintable(error));
        QCOMPARE(w.session().aliases().table.rows.size(), std::size_t{1});
        aliases->close();
        channels.search().clear();
        QTest::keyClicks(&channels.search(), QStringLiteral("ground"));
        QTRY_VERIFY_WITH_TIMEOUT(channels.visibleChannels().contains(QStringLiteral("Ground Speed")),
                                 kTimeoutMs);
        channels.search().clear();

        // Mappings: STEERANGLE is called "Steering Angle" from the next open.
        editor.openMappings();
        MappingsDialog* mappings = editor.mappingsDialog();
        QVERIFY(mappings != nullptr);
        project::Mappings m = mappings->document();
        m.rows.push_back(project::Mappings::Row{"STEERANGLE", "Steering Angle", std::nullopt, false});
        mappings->setDocument(m);
        QVERIFY2(mappings->apply(&error), qPrintable(error));
        mappings->close();
        editor.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // Channel Status (CTRL+U): the built-in Corr Speed is OK; the alias's row carries its resolution
        // trace (Ground Speed -> alias -> SPEED).
        w.returnFocus();
        key(w, QStringLiteral("Ctrl+U"));
        ChannelStatusWindow& status = w.channelStatusWindow();
        QTRY_VERIFY(w.docks().isVisible(QStringLiteral("channelstatus")));
        QTRY_VERIFY_WITH_TIMEOUT(status.selectRow(QStringLiteral("Corr Speed")), kTimeoutMs);
        QTRY_VERIFY2_WITH_TIMEOUT(status.detailPane().toPlainText().contains(QStringLiteral("Status: OK")),
                                  qPrintable(status.detailPane().toPlainText()), kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(status.selectRow(QStringLiteral("Ground Speed")), kTimeoutMs);
        QTRY_VERIFY2_WITH_TIMEOUT(status.detailPane().toPlainText().contains(QStringLiteral("alias")) &&
                                      status.detailPane().toPlainText().contains(QStringLiteral("'SPEED'")),
                                  qPrintable(status.detailPane().toPlainText()), kTimeoutMs);

        // The mapping applies at open: close the log (CTRL+A, file.close_all) and open it again.
        QVERIFY(w.commands().invoke(QStringLiteral("file.close_all")));
        QTRY_VERIFY_WITH_TIMEOUT(w.selection().sources().empty(), kTimeoutMs);
        QVERIFY(d.openDay());
        const auto reopened = sourceNamed(w, "saturday.ld");
        QVERIFY(channelOf(*reopened, "Steering Angle") != nullptr);

        // The Track Map dock: shown with the venue's map (generated from GPS at the first open).
        const bool trackShown = w.docks().isVisible(QStringLiteral("trackmap"));
        QVERIFY(w.commands().invoke(QStringLiteral("view.track_map_window")));
        QCOMPARE(w.docks().isVisible(QStringLiteral("trackmap")), !trackShown);
        if (!w.docks().isVisible(QStringLiteral("trackmap"))) {
            QVERIFY(w.commands().invoke(QStringLiteral("view.track_map_window")));
        }
        d.selectMainAndReference();
        QTRY_VERIFY_WITH_TIMEOUT(w.trackMapWindow().trackPosition() != nullptr &&
                                     w.trackMapWindow().trackPosition()->trackMap() != nullptr,
                                 kTimeoutMs);
    }

    // ---- 7: maths ----------------------------------------------------------------------------------------
    void item07_mathsGlobalLocalTransientPendingEditor() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        QUndoStack& stack = w.session().undoStack();
        const double P = kLapSeconds[kFastest];
        const Micros at = d.mainStart() + static_cast<Micros>(std::llround(kCursorU * P * 1e6));
        const double v = speedMs(circuit_.lengthM, P, kCursorU);

        // CTRL+M: the Maths Editor; the module file imported as a global module (one step).
        const std::filesystem::path file = d.logs / "saturday.maths.json";
        {
            std::ofstream out(file, std::ios::binary);
            out << kSaturdayModule;
        }
        w.returnFocus();
        key(w, QStringLiteral("Ctrl+M"));
        MathsEditor& editor = w.mathsEditor();
        QTRY_VERIFY(editor.isVisible());
        const int steps = stack.count();
        QString error;
        QVERIFY2(editor.importModule(file, &error), qPrintable(error));
        QCOMPARE(stack.count(), steps + 1);
        QCOMPARE(w.session().mathsModules().size(), std::size_t{1});

        // Every item evaluates when asked for (Pending, then Ok) to the formula's value at the cursor.
        for (const char* name : {"Throttle Doubled", "Pedal State", "G Force Long", "Gear Factor"}) {
            QTRY_VERIFY2_WITH_TIMEOUT(
                mathsValueAt(w, d.src, name, at).has_value(),
                qPrintable(QLatin1String(name) + QStringLiteral(": ") + mathsStatusText(w, d.src, name)),
                kTimeoutMs);
            QVERIFY2(mathsOk(w, d.src, name), qPrintable(mathsStatusText(w, d.src, name)));
        }
        std::optional<double> x;
        QTRY_VERIFY_WITH_TIMEOUT((x = mathsValueAt(w, d.src, "Throttle Doubled", at)).has_value(),
                                 kTimeoutMs);
        QVERIFY2(std::abs(*x - 2.0 * throttlePct(kCursorU)) < 0.01, qPrintable(QString::number(*x)));
        QTRY_VERIFY_WITH_TIMEOUT((x = mathsValueAt(w, d.src, "Pedal State", at)).has_value(), kTimeoutMs);
        QCOMPARE(*x, 1.0); // on the throttle
        const std::optional<model::ChannelDescriptor> state =
            w.maths().descriptor(maths::ChannelKey::make(d.src, model::kNormalLayer, "Pedal State"));
        QVERIFY(state.has_value() && state->kind == store::SampleKind::Enum); // an enumerated channel
        QTRY_VERIFY_WITH_TIMEOUT((x = mathsValueAt(w, d.src, "Gear Factor", at)).has_value(), kTimeoutMs);
        QCOMPARE(*x, 10.0 * gearOf(v));
        // The 6-point filter shadows the logged G Force Long: a moving average of a smooth signal (base
        // m/s2).
        const double longG = circuit_.lengthM / P * 0.3 * (2.0 * std::numbers::pi / P) *
                             std::cos(2.0 * std::numbers::pi * kCursorU);
        QTRY_VERIFY_WITH_TIMEOUT((x = mathsValueAt(w, d.src, "G Force Long", at)).has_value(), kTimeoutMs);
        QVERIFY2(std::abs(*x - longG) < 0.02 * std::abs(longG) + 1e-3, qPrintable(QString::number(*x)));
        // Channel Status lists the module's items.
        QVERIFY(w.commands().invoke(QStringLiteral("tools.channel_status")));
        QTRY_VERIFY_WITH_TIMEOUT(
            w.channelStatusWindow().tableText().contains(QStringLiteral("Throttle Doubled")), kTimeoutMs);

        // Live diagnostics: a broken expression is reported in the editor and refused by Apply.
        QVERIFY(editor.openAt(QStringLiteral("saturday"), QStringLiteral("Throttle Doubled")));
        editor.expressionEdit().setPlainText(QStringLiteral("'THROTTLE'[%] * ("));
        editor.flushDiagnostics();
        QVERIFY(!editor.currentDiagnostics().empty());
        QVERIFY(!editor.apply());
        editor.revert();
        QVERIFY(!editor.isPageDirty());
        editor.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // From the channel label (the Braking graph's active channel BRAKE): CTRL+SHIFT+O adds a local maths
        // item (sidecar), +5 %; CTRL+SHIFT+F a transient filter (never saved).
        w.selectWorksheet(worksheetNamed(w, QStringLiteral("Braking")));
        TimeDistanceGraph* g = uitest::graphOf(w);
        QVERIFY(g != nullptr);
        QCOMPARE(g->activeChannel().value_or(QString()), QStringLiteral("BRAKE"));
        ActiveChannelCommands& active = w.activeChannelCommands();
        active.setScaleOffsetPrompt([](const QString&, const QString&) {
            ActiveChannelCommands::ScaleOffsetChoice c;
            c.scale = 1.0;
            c.offset = 5.0;
            c.offsetUnit = QStringLiteral("%");
            return std::optional<ActiveChannelCommands::ScaleOffsetChoice>(c);
        });
        active.setFilterPrompt([](const QString&) {
            ActiveChannelCommands::FilterChoice c;
            c.points = 5;
            return std::optional<ActiveChannelCommands::FilterChoice>(c);
        });
        focusComponent(w, g);
        const int beforeLocal = stack.count();
        key(w, QStringLiteral("Ctrl+Shift+O"));
        QCOMPARE(stack.count(), beforeLocal + 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            sourceNamed(w, "saturday.ld")->sidecar.layer("Normal").localMaths.has_value(), kTimeoutMs);
        // BRAKE is 0 at u = 0.125; +5 % is 0.05 in the base unit (ratio).
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(mathsValueAt(w, d.src, "BRAKE", at).value_or(-1.0) - 0.05) < 1e-4,
                                 kTimeoutMs);
        focusComponent(w, g);
        key(w, QStringLiteral("Ctrl+Shift+F"));
        QTRY_VERIFY_WITH_TIMEOUT(active.transientModule(d.src).has_value() &&
                                     !active.transientModule(d.src)->items.empty(),
                                 kTimeoutMs);
        QCOMPARE(stack.count(), beforeLocal + 1); // transient: not a document edit
        QTRY_VERIFY_WITH_TIMEOUT(mathsOk(w, d.src, "BRAKE"), kTimeoutMs);
    }

    // ---- 8: corrected speed and distance, lap stretching, Lap / Details / Track editors
    // --------------------
    void item08_correctedSpeedLapStretchLapDetailsTrackEditors() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        QUndoStack& stack = w.session().undoStack();
        const double L = circuit_.lengthM;
        const double P = kLapSeconds[kFastest];
        const Micros at = d.mainStart() + static_cast<Micros>(std::llround(kCursorU * P * 1e6));
        const double v = speedMs(L, P, kCursorU);

        // Corr Speed is the faster wheel (FL = v); the dialog's alternate-list method, front right first,
        // gives 0.97 v.
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(mathsValueAt(w, d.src, "Corr Speed", at).value_or(-1.0) - v) < 0.005 * v, kTimeoutMs);
        QVERIFY(w.commands().invoke(QStringLiteral("tools.corrected_speed")));
        CorrectedSpeedDialog& corrected = w.correctedSpeedDialog();
        QTRY_VERIFY(corrected.isVisible());
        analysis::CorrectedSpeedSettings settings = corrected.settings();
        settings.method = analysis::SpeedMethod::AlternateList;
        settings.alternateListChannels = {"Wheel Speed FR", "Wheel Speed FL"};
        corrected.setSettings(settings);
        const int steps = stack.count();
        QVERIFY2(corrected.validationMessage().isEmpty(), qPrintable(corrected.validationMessage()));
        corrected.apply();
        QCOMPARE(stack.count(), steps + 1);
        QVERIFY(!w.session().correctedSpeedSettings().isNull());
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(mathsValueAt(w, d.src, "Corr Speed", at).value_or(-1.0) - 0.97 * v) < 0.005 * v,
            kTimeoutMs);
        stack.undo();
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(mathsValueAt(w, d.src, "Corr Speed", at).value_or(-1.0) - v) < 0.005 * v, kTimeoutMs);
        corrected.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // Lap stretching from the toolbar toggle: every lap measures the circuit's length (the speed
        // integrates to L per lap), so each is within the tolerance; the Lap Editor shows the per-lap report.
        // The report is read as reportChanged announces it (polling report() would itself ask for the lap
        // indexes): the undo above brought Corr Dist back to an earlier version, so every lap's index is
        // built again, one pool task per lap, and the report is final once each lap's length is in.
        qt::LapStretchService& stretch = w.lapStretch();
        std::vector<analysis::LapStretchRow> announced;
        auto receiver = std::make_unique<QObject>();
        QObject::connect(&stretch, &qt::LapStretchService::reportChanged, receiver.get(),
                         [&](const model::SourceId& id) {
                             if (id == d.src) {
                                 announced = stretch.report(id);
                             }
                         });
        const auto everyLapMeasuresTheCircuit = [&] {
            return announced.size() >= kLapSeconds.size() - 2 &&
                   std::all_of(announced.begin(), announced.end(), [&](const analysis::LapStretchRow& row) {
                       return std::abs(row.lengthM - L) < 0.005 * L;
                   });
        };
        const auto lengths = [&] {
            QStringList out;
            for (const analysis::LapStretchRow& row : announced) {
                out << QString::number(row.lengthM);
            }
            return out.join(QStringLiteral(", "));
        };
        const bool stretchWasOn = stretch.enabled();
        QVERIFY(w.commands().invoke(QStringLiteral("data.lap_stretch_toggle")));
        QCOMPARE(stretch.enabled(), !stretchWasOn);
        if (!stretch.enabled()) {
            QVERIFY(w.commands().invoke(QStringLiteral("data.lap_stretch_toggle")));
        }
        QVERIFY(stretch.enabled());
        QTRY_VERIFY2_WITH_TIMEOUT(everyLapMeasuresTheCircuit(), qPrintable(lengths()), kTimeoutMs);
        receiver.reset();
        QVERIFY(w.commands().invoke(QStringLiteral("tools.lap_editor")));
        LapEditor& laps = w.lapEditor();
        QTRY_VERIFY(laps.isVisible());
        laps.setSource(d.src);
        QTRY_COMPARE(laps.rowCount(), static_cast<int>(kLapSeconds.size()));
        QVERIFY(!laps.stretchReportText().isEmpty());
        // Trust and a beacon from the Lap Editor (undoable).
        const int lapSteps = stack.count();
        const int row70 = laps.rowOf(d.laps[1]);
        QVERIFY(row70 >= 0);
        QVERIFY(laps.setTrusted(row70, false));
        QTRY_VERIFY_WITH_TIMEOUT(!w.selection().resolve(d.laps[1])->trusted, kTimeoutMs);
        QVERIFY(laps.insertBeaconAt(static_cast<Micros>((lapStart(1) + 35.0) * 1e6)));
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().displayedRanges(d.src).size(), kLapSeconds.size() + 1,
                                  kTimeoutMs);
        while (stack.count() > lapSteps && stack.canUndo()) {
            stack.undo();
        }
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().displayedRanges(d.src).size(), kLapSeconds.size(),
                                  kTimeoutMs);
        if (stretch.enabled() != stretchWasOn) {
            QVERIFY(w.commands().invoke(QStringLiteral("data.lap_stretch_toggle")));
        }
        QCOMPARE(stretch.enabled(), stretchWasOn);
        laps.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // The Details editor (CTRL+D): the driver edited, the source's details follow; undo restores it.
        w.returnFocus();
        key(w, QStringLiteral("Ctrl+D"));
        DetailsEditor& details = w.detailsEditor();
        QTRY_VERIFY(details.isVisible());
        QVERIFY(details.detailIds().contains(QStringLiteral("Driver")));
        QCOMPARE(details.cellText(QStringLiteral("Driver"), 0), QString::fromStdString(kDriver));
        QVERIFY(details.setCell(QStringLiteral("Driver"), 0, QStringLiteral("Sunday Driver")));
        QTRY_VERIFY_WITH_TIMEOUT(
            details.cellText(QStringLiteral("Driver"), 0) == QStringLiteral("Sunday Driver"), kTimeoutMs);
        QVERIFY(details.isEdited(QStringLiteral("Driver"), 0));
        stack.undo();
        QTRY_VERIFY_WITH_TIMEOUT(
            details.cellText(QStringLiteral("Driver"), 0) == QString::fromStdString(kDriver), kTimeoutMs);
        details.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        // The Track Editor: the map generated from GPS at the open, then both methods on the Main lap (GPS;
        // speed and lateral G), each about the circuit's length; Accept keeps the default sections; a user
        // section is added.
        QTRY_VERIFY_WITH_TIMEOUT(w.trackMaps().mapFor(*w.selection().source(d.src)) != nullptr, kTimeoutMs);
        QVERIFY(w.commands().invoke(QStringLiteral("tools.track_editor")));
        TrackEditor& track = w.trackEditor();
        QTRY_VERIFY(track.isVisible());
        QVERIFY(track.setLap(d.laps[kFastest]));
        track.setMethod(analysis::TrackMethod::SpeedLatG);
        track.setLateralChannel(QStringLiteral("G Force Lat"));
        track.generate();
        QTRY_VERIFY_WITH_TIMEOUT(!track.generating() && track.preview() != nullptr, kTimeoutMs);
        QVERIFY2(std::abs(track.preview()->lengthM - L) < 0.05 * L,
                 qPrintable(QString::number(track.preview()->lengthM)));
        track.setMethod(analysis::TrackMethod::Gps);
        track.generate();
        QTRY_VERIFY_WITH_TIMEOUT(!track.generating() && track.preview() != nullptr &&
                                     track.preview()->generatedFrom.method == analysis::TrackMethod::Gps,
                                 kTimeoutMs);
        QVERIFY(std::abs(track.preview()->lengthM - L) < 0.02 * L);
        track.accept();
        QTRY_VERIFY_WITH_TIMEOUT(track.currentMap() != nullptr && !track.currentMap()->sections.empty(),
                                 kTimeoutMs);
        QCOMPARE(track.venue(), QString::fromStdString(kVenue));
        QVERIFY(track.addSection(QStringLiteral("T1 entry"), 100.0, 160.0, QStringLiteral("corner")));
        bool user = false;
        for (const analysis::TrackSection& s : track.currentMap()->sections) {
            user = user || (s.name == "T1 entry" && s.userDefined);
        }
        QVERIFY(user);
        track.close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
    }

    // ---- 9: the hotkey table (data/keymaps/default.json) and the command palette
    // ---------------------------
    void item09_hotkeyTableAndCommandPalette() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        const QJsonObject keymap = jsonFile(qt::test::repoRoot() / "data" / "keymaps" / "default.json");
        const QJsonArray rows = keymap.value(QStringLiteral("bindings")).toArray();
        QVERIFY(rows.size() >= 170);
        QSet<QString> commands;
        int implemented = 0;
        for (const QJsonValue& value : rows) {
            const QJsonObject row = value.toObject();
            const QString id = row.value(QStringLiteral("command")).toString();
            const QString status = row.value(QStringLiteral("status")).toString();
            const std::optional<qt::CommandContext> context =
                qt::parseCommandContext(row.value(QStringLiteral("context")).toString());
            commands.insert(id);
            QVERIFY2(context.has_value(), qPrintable(id));
            QVERIFY2(w.commands().find(id) != nullptr, qPrintable(id + QStringLiteral(" is not registered")));
            if (status != QLatin1String("implemented")) {
                QVERIFY2(!w.commands().isEnabled(id),
                         qPrintable(id + QStringLiteral(" is enabled but ") + status));
                continue;
            }
            ++implemented;
            // Bound in its context: each key resolves to this command (with its arguments).
            for (const QJsonValue& k : row.value(QStringLiteral("keys")).toArray()) {
                const std::optional<qt::KeyResolution> r =
                    w.keymap().resolve(qt::KeymapBinder::normalise(k.toString()), {*context});
                QVERIFY2(r.has_value() && r->command == id,
                         qPrintable(id + QStringLiteral(": ") + k.toString() + QStringLiteral(" -> ") +
                                    (r ? r->command : QStringLiteral("nothing"))));
                QCOMPARE(QJsonArray::fromVariantList(r->args), row.value(QStringLiteral("args")).toArray());
            }
        }
        QVERIFY(implemented >= 170);
        // The table is 06 section 14.4's, command for command.
        QCOMPARE(commands, QSet<QString>(kKeymapCommands.begin(), kKeymapCommands.end()));

        // The command palette (CTRL+SHIFT+P): it lists the commands with their keys and runs them.
        focusComponent(w, uitest::graphOf(w));
        key(w, QStringLiteral("Ctrl+Shift+P"));
        CommandPalette& palette = w.commandPalette();
        QTRY_VERIFY(palette.isVisible());
        QCOMPARE(palette.keyText(QStringLiteral("data.select_fastest")), QStringLiteral("Ctrl+F"));
        QVERIFY(!w.selection().snapshot().singleColourMain);
        QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("single colour"));
        QVERIFY(!palette.visibleIds().isEmpty());
        QCOMPARE(palette.visibleIds().front(), QStringLiteral("data.single_colour_main"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(w.selection().snapshot().singleColourMain);
        QTRY_VERIFY(!palette.isVisible());
        // From the Data window's context: its rows are offered there.
        w.docks().focus(QStringLiteral("data"));
        w.dataWindow().highlight(d.laps[1]);
        key(w, QStringLiteral("Ctrl+Shift+P"));
        QTRY_VERIFY(palette.isVisible());
        QCOMPARE(palette.capturedContext(), qt::CommandContext::DataWindow);
        QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("set main"));
        QTRY_VERIFY(palette.visibleIds().contains(QStringLiteral("data.select_main")));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_VERIFY(!palette.isVisible());
    }

    // ---- 10: the raster backend on 20 traces; HiDPI at DPR 1 and 2
    // -------------------------------------------
    void item10_rasterTwentyTracesAndHiDpiAtDpr1And2() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        QVERIFY(d.openDay());
        d.selectMainAndReference();
        MainWindow& w = *d.w;
        QCOMPARE(w.devicePixelRatioF(), 1.0);
        // A graph of 20 traces (five groups of four) over the whole worksheet "Driver".
        w.selectWorksheet(worksheetNamed(w, QStringLiteral("Driver")));
        const QStringList names{"Wheel Speed FL", "Wheel Speed FR", "SPEED",          "Corr Speed",
                                "THROTTLE",       "BRAKE",          "GEAR",           "RPMS",
                                "STEERANGLE",     "G Force Lat",    "G Force Long",   "Corr Dist",
                                "GPS Latitude",   "GPS Longitude",  "Wheel Speed FL", "THROTTLE",
                                "BRAKE",          "RPMS",           "G Force Lat",    "SPEED"};
        QJsonArray groups;
        for (int gi = 0; gi < 5; ++gi) {
            QJsonArray traces;
            for (int t = 0; t < 4; ++t) {
                traces.append(QJsonObject{{QStringLiteral("channel"), names.at(gi * 4 + t)}});
            }
            groups.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Group %1").arg(gi + 1)},
                                      {QStringLiteral("scaleMode"), QStringLiteral("channel")},
                                      {QStringLiteral("traces"), traces}});
        }
        TimeDistanceGraph* g = uitest::graphOf(w);
        QVERIFY(g != nullptr);
        {
            project::ComponentSpec spec = g->spec();
            QJsonObject body = bodyOf(*g);
            body.insert(QStringLiteral("groups"), groups);
            spec.body =
                JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString()).value();
            qt::ProjectSession& s = w.session();
            project::WorkbookFile book = s.workbooks().at(static_cast<std::size_t>(s.activeWorkbook()));
            project::WorksheetDef& sheet =
                book.worksheets.at(static_cast<std::size_t>(w.activeWorksheetIndex()));
            for (project::ComponentSpec& c : sheet.components) {
                if (c.id == spec.id) {
                    c = spec;
                }
            }
            s.undoStack().push(new qt::WorkbookCommand(s, s.activeWorkbook(), std::move(book),
                                                       QStringLiteral("Twenty traces")));
        }
        g = uitest::graphOf(w);
        QVERIFY(g != nullptr);
        QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
        int traces = 0;
        for (const plot::TdGroupLayout& gl : g->layout().groups) {
            traces += static_cast<int>(gl.traces.size());
        }
        QCOMPARE(traces, 20);
        // A cursor move repaints the Dynamic layer only; a zoom step rebuilds the Static layer.
        const int staticBuilds = g->staticBuildsSubmitted();
        const int dynamicBuilds = g->dynamicBuilds();
        w.activeWorksheet()->setCursor(10 * kSec, qt::CursorSource::Programmatic);
        QTRY_VERIFY_WITH_TIMEOUT(g->dynamicBuilds() > dynamicBuilds, kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
        QCOMPARE(g->staticBuildsSubmitted(), staticBuilds);
        focusComponent(w, g);
        key(w, QStringLiteral("Up")); // zoom in about the cursor
        QTRY_VERIFY_WITH_TIMEOUT(g->staticBuildsSubmitted() > staticBuilds, kTimeoutMs);
        QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
        const QSize logical = g->size();
        QCOMPARE(g->layers().staticImage().size(), logical);

        // HiDPI: the window on the DPR 2 screen re-renders the graph at device pixels; back at DPR 1 as
        // before.
        for (const qreal dpr : {2.0, 1.0}) {
            const QSize size = w.size();
            QWindow* window = w.windowHandle();
            window->setScreen(dprtest::screenForDpr(dpr));
            w.move(dprtest::pointOn(dpr, QPoint(40, 40)));
            // As tests/ui/tst_dpr_switch.cpp moveTo: the platform reports the move, then the logical size is
            // set again (the offscreen platform keeps the native size where Windows keeps the logical one).
            QTRY_VERIFY_WITH_TIMEOUT(w.devicePixelRatioF() == dpr && w.size() == window->size(), kTimeoutMs);
            w.resize(size);
            QTRY_VERIFY_WITH_TIMEOUT(w.size() == size && window->size() == size, kTimeoutMs);
            g = uitest::graphOf(w);
            QTRY_VERIFY_WITH_TIMEOUT(
                g->layoutDevicePixelRatio() == dpr && g->layers().devicePixelRatio() == dpr, kTimeoutMs);
            QVERIFY(uitest::waitQuiet(*g, kTimeoutMs));
            const QImage& image = g->layers().staticImage();
            QCOMPARE(image.devicePixelRatio(), dpr);
            QCOMPARE(image.size(), QSize(static_cast<int>(std::lround(g->width() * dpr)),
                                         static_cast<int>(std::lround(g->height() * dpr))));
            QCOMPARE(static_cast<qreal>(g->layout().dpr), dpr);
        }
    }

    // ---- The demo of 07 section 7.5, offscreen, without the installer (the VM checklist's)
    // ------------------
    //   esses -w <project> -l <log>; the log is above the memory budget: Messages warns with the estimate
    //   before anything is loaded, Continue, the Performance line shows the resident bytes against the
    //   budget; Help > About with the Qt notice and the source link; a portable layout keeps the settings
    //   beside the executable.
    void demo75_offscreenWithoutTheInstaller() {
        Day d;
        QVERIFY(d.start(masterDir(), true));
        const std::filesystem::path folder = d.folder;
        QVERIFY(d.w->close());
        d.w.reset();
        d.rt->cache().setBudgetBytes(std::size_t{64} << 10); // 64 KiB: the day's log is above it
        // esses -w <project> -l <log> (apps/app/src/main.cpp passes them to MainWindow::openProject).
        d.w = std::make_unique<MainWindow>(*d.rt, uitest::pathsFor(d.config.path()));
        d.w->setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
        MainWindow& w = *d.w;
        QVERIFY(w.openProject(folder, {d.ld}));
        QVERIFY(uitest::showAndActivate(w));
        QVERIFY(w.session().isOpen());
        QTRY_COMPARE_WITH_TIMEOUT(w.loader().pendingBudgetOpens(), 1, kTimeoutMs);
        int row = -1;
        for (int r = w.messages().rowCount() - 1; r >= 0 && row < 0; --r) {
            if (w.messages().at(r).code == QLatin1String(BudgetShell::kWarningCode)) {
                row = r;
            }
        }
        QVERIFY(row >= 0);
        const qt::Message warning = w.messages().at(row);
        QCOMPARE(warning.origin, qt::Message::Origin::Budget);
        QVERIFY2(warning.text.contains(QStringLiteral("needs about")) &&
                     warning.text.contains(QStringLiteral("above the 64 KB memory budget")),
                 qPrintable(warning.text));
        QVERIFY(w.selection().sources().empty()); // nothing published before the answer
        QVERIFY(w.messagesWindow().invokeAction(
            w.messagesWindow().filterModel().mapFromSource(w.messages().index(row))));
        QTRY_COMPARE_WITH_TIMEOUT(w.selection().sources().size(), std::size_t{1}, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(d.quiet(), kTimeoutMs);
        BudgetShell* budget = shellExtension<BudgetShell>(w);
        QVERIFY(budget != nullptr && budget->performanceLabel() != nullptr);
        budget->monitor()->refresh();
        QVERIFY2(budget->performanceLabel()->text().startsWith(QStringLiteral("Resident ")) &&
                     budget->performanceLabel()->text().contains(QStringLiteral("of 64 KB")),
                 qPrintable(budget->performanceLabel()->text()));

        // Help > About: the Qt LGPL notice with the source archive of the Qt in use, and this build's source.
        QVERIFY(w.commands().invoke(QStringLiteral("help.about")));
        AboutShell* about = shellExtension<AboutShell>(w);
        QVERIFY(about != nullptr);
        QTRY_VERIFY(about->dialog() != nullptr && about->dialog()->isVisible());
        const QString text = about->dialog()->text();
        const QString qtVersion = QString::fromLatin1(qVersion());
        QVERIFY2(text.contains(QStringLiteral("LGPL")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("https://download.qt.io/official_releases/qt/%1/%2/")
                                   .arg(qtVersion.section(QLatin1Char('.'), 0, 1), qtVersion)),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("https://github.com/mitchdetailed/jubilant-octo-meme")),
                 qPrintable(text));
        about->dialog()->close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QVERIFY(w.close());
        d.w.reset();

        // The portable layout: portable.marker beside the executable keeps the settings in <exe dir>/config.
        const std::filesystem::path stick = fsPath(d.root.path()) / "usb";
        std::filesystem::create_directories(stick);
        std::ofstream(stick / "portable.marker", std::ios::binary) << "";
        PathOverrides overrides;
        overrides.exeDir = stick;
        overrides.dataDir = qt::test::repoRoot() / "data";
        const Result<Paths> portable = Paths::resolve(overrides);
        QVERIFY(portable.ok() && portable->portable());
        QVERIFY(portable->configDir() == stick / "config");
        {
            MainWindow p(*d.rt, *portable);
            p.setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
            QVERIFY(uitest::showAndActivate(p));
            QVERIFY(p.schemeSelector().selectScheme(QStringLiteral("Esses Dark")));
            QVERIFY(p.close());
        }
        QVERIFY(std::filesystem::exists(stick / "config"));
        std::error_code ec;
        std::size_t written = 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(stick / "config", ec)) {
            written += e.is_regular_file() ? 1 : 0;
        }
        QVERIFY(written >= 1); // the settings (and the layout) beside the executable
    }

private:
    std::filesystem::path masterDir() const { return fsPath(master_.path()); }

    QTemporaryDir master_; // the day's .ld + .ldx, written once
    QTemporaryDir extra_;  // the two CSV files
    Circuit circuit_;
    std::filesystem::path legacyCsv_;
    std::filesystem::path unknownCsv_;
};

QTEST_MAIN(TstWalkthroughSaturday)
#include "tst_walkthrough_saturday.moc"
