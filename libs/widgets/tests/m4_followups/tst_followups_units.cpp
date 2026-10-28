// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M4 followups of esses_widgets, units and files (D-211 (a)-(d); docs/milestones/M3.md section 6 items 19,
// 22, 24 and 26), offscreen:
//   (a) a channel without a display unit shows in its quantity's default display unit: a fraction stored in
//       "ratio" reads in % in the Values model, the scatter's axes and the channel report, and a unit the
//       component names (the scatter's x / y, a report channel's) wins;
//   (b) the Properties pages pick display units from the units of the channel's quantity (a combo that
//       follows the channel field; compat units left out), in every type that has one;
//   (c) the Track Editor's speed-class column and its bounds in the Corr Dist display unit;
//   (d) a component's Export Data in the legacy CSV dialect, values in the display unit.
// Expectations come from the design and data/units.json read by hand (Ratio: ratio, % at x100, Trim and
// %Trim legacy; Speed: m/s, km/h x3.6, mph, ft/s, mm/s, in/s; Distance: m, km, ...), and from the synthetic
// sources' formulas (a "Duty" channel stored in ratio, 0.25 on even samples and 0.75 on odd ones).

#include "components/plots_test_support.hpp"
#include "components/reports_track/rt_support.hpp"
#include "m4_followups/legacy_csv_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/numeric_gauge.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QUndoStack>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;
using namespace esses::widgets::plotstest;

namespace {

const QStringList kRatioUnits{QString(), QStringLiteral("ratio"), QStringLiteral("%")};
const QStringList kSpeedUnits{QString(),
                              QStringLiteral("m/s"),
                              QStringLiteral("km/h"),
                              QStringLiteral("mph"),
                              QStringLiteral("ft/s"),
                              QStringLiteral("mm/s"),
                              QStringLiteral("in/s")};

// The synthetic source of component_test_support with a "Duty" channel stored in the Ratio base unit.
std::shared_ptr<const model::DataSource> withDuty(World& w) {
    auto src = std::make_shared<model::DataSource>(*makeSource(SourceOptions{}, w.rt.cache()));
    model::DataLayer& layer = src->layers.front();
    const units::UnitRegistry& reg = *shippedUnits();
    const Rate rate{50, 1};
    const auto n = static_cast<std::size_t>(layer.duration / 20'000);
    std::vector<float> duty(n);
    for (std::size_t i = 0; i < n; ++i) {
        duty[i] = i % 2 == 0 ? 0.25F : 0.75F;
    }
    model::Channel c;
    c.desc.name = "Duty";
    c.desc.loggedName = "Duty";
    c.desc.id = model::channelIdLogged(src->id, model::kNormalLayer, "Duty", "test");
    c.desc.unit = *reg.find("ratio");
    c.desc.quantity = reg.quantityOf(c.desc.unit);
    c.desc.dtype = store::Dtype::F32;
    c.desc.rate = rate;
    c.desc.dps = 3;
    store::ColumnOptions co;
    co.cache = &w.rt.cache();
    c.series = store::UniformSeries{
        rate, 0, store::Column(store::InMemoryProvider::fromF32(duty, {}, "memory:Duty"), co)};
    layer.addChannel(std::move(c));
    std::shared_ptr<const model::DataSource> out = std::move(src);
    w.maths.addSource(out);
    w.distance.prefetch(out->id);
    w.selection.sourcePublished(out);
    return out;
}

QComboBox* combo(PropertiesDialog& d, const char* pointer) {
    return qobject_cast<QComboBox*>(d.binder().editorFor(QString::fromLatin1(pointer)));
}

std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::path(ESSES_TEST_BINARY_DIR) / "followups-scratch" /
                                      (name + "-" + std::to_string(QCoreApplication::applicationPid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

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

// A project with the synthetic circuit (the Track Editor's world in tst_track_editor).
struct EditorWorld {
    World w;
    std::filesystem::path dir;
    qt::ProjectSession session;
    qt::TrackMapService maps;
    std::shared_ptr<const model::DataSource> src;

    explicit EditorWorld(const std::string& name)
        : dir(scratch(name)), session(pathsFor(dir / "config")),
          maps(&session, &w.maths, &w.rt.dispatcher()) {
        if (!session.loadDefaults().ok() || !session.newProject(dir / "proj", "p").ok()) {
            qFatal("cannot create the project");
        }
        src = rtw::publish(w, rtw::circuitSource(repoRoot() / "fixtures" / "track" / "synthetic-circuit",
                                                 "Synthetic Circuit", *shippedUnits(), w.rt.cache()));
        w.selection.setMain(rtw::refOf(*src, rtw::kLaps, "Lap 1"));
    }
};

int columnTitled(const QTableWidget& t, const QString& title) {
    for (int c = 0; c < t.columnCount(); ++c) {
        if (t.horizontalHeaderItem(c) != nullptr && t.horizontalHeaderItem(c)->text() == title) {
            return c;
        }
    }
    return -1;
}

int rowNamed(const QTableWidget& t, const QString& name) {
    for (int r = 0; r < t.rowCount(); ++r) {
        if (t.item(r, 0) != nullptr && t.item(r, 0)->text() == name) {
            return r;
        }
    }
    return -1;
}

void setOverrideUnit(qt::ProjectSession& session, const std::string& channel, const std::string& unit) {
    project::ChannelOverrides::Entry e;
    if (const project::ChannelOverrides::Entry* now = session.overrides().find(channel)) {
        e = *now;
    }
    e.displayUnit = unit;
    session.undoStack().push(new qt::SetOverrideCommand(session, channel, e));
}

} // namespace

class TstFollowupsUnits : public QObject {
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

    // (a) M3.md item 19: a fraction stored in ratio reads in % wherever no unit is named.
    void ratioChannelsShowInPercent() {
        World w;
        const auto src = withDuty(w);
        selectMain(w, lapRef(*src, 1)); // cursor at the lap start: sample 2000 (even), 0.25
        qt::ValuesModel values(w.selection, w.maths);
        values.setWorksheet(w.view.get());
        values.setChannels({QStringLiteral("Duty"), QStringLiteral("Throttle")});
        QTRY_COMPARE_WITH_TIMEOUT(values.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("25.000"),
                                  kTimeoutMs);
        QCOMPARE(values.cellText(0, qt::ValuesModel::UnitCol), QStringLiteral("%"));
        QCOMPARE(values.cellText(1, qt::ValuesModel::UnitCol), QStringLiteral("%")); // stored in %: as stored

        // The scatter's channels: x named "ratio", y unnamed (the default, %), a second y named km/h.
        const project::ComponentSpec scatter =
            specOf("scatter",
                   R"({"xChannel":"Duty","xDisplayUnit":"ratio","traces":[{"yChannel":"Duty"},)"
                   R"({"yChannel":"Wheel Speed FL","displayUnit":"km/h"},{"yChannel":"Wheel Speed FR"}]})");
        const plot::ComponentBuildInputs in =
            w.render.prepareInputs(scatter, w.view->state(), w.selection.snapshot(),
                                   plot::LayoutContext{400, 300, 1.0F}, w.scheme, {});
        QVERIFY(!in.slots.empty());
        const std::vector<plot::ResolvedChannel>& ch = in.slots.front().channels;
        QCOMPARE(ch.size(), std::size_t{4});
        QCOMPARE(QString::fromStdString(ch[0].displayUnit), QStringLiteral("ratio"));
        QCOMPARE(ch[0].toDisplay(0.25), 0.25);
        QCOMPARE(QString::fromStdString(ch[1].displayUnit), QStringLiteral("%"));
        QCOMPARE(ch[1].toDisplay(0.25), 25.0);
        QCOMPARE(QString::fromStdString(ch[2].displayUnit), QStringLiteral("km/h"));
        QCOMPARE(ch[2].toDisplay(10.0), 36.0);
        QCOMPARE(QString::fromStdString(ch[3].displayUnit),
                 QStringLiteral("m/s")); // other quantities as stored

        // The channel report: a channel's displayUnit names its column's unit; Duty defaults to %.
        const QJsonObject body{
            {QStringLiteral("report"),
             QJsonObject{
                 {QStringLiteral("scope"), QStringLiteral("selectedRanges")},
                 {QStringLiteral("headers"), QJsonArray{QStringLiteral("Outings:Laps")}},
                 {QStringLiteral("channels"),
                  QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Wheel Speed FL")},
                                         {QStringLiteral("stats"), QJsonArray{QStringLiteral("max")}},
                                         {QStringLiteral("displayUnit"), QStringLiteral("km/h")}},
                             QJsonObject{{QStringLiteral("name"), QStringLiteral("Duty")},
                                         {QStringLiteral("stats"), QJsonArray{QStringLiteral("max")}}}}}}}};
        std::unique_ptr<ComponentWidget> owner(
            ComponentWidget::create(rtw::componentSpec(ChannelReport::kType, body), *w.context));
        auto* report = qobject_cast<ChannelReport*>(owner.get());
        QVERIFY(report != nullptr);
        report->resize(520, 220);
        report->show();
        QVERIFY(rtw::settle(report->model(), 0));
        QTRY_COMPARE_WITH_TIMEOUT(report->model().pendingCells(), 0, kTimeoutMs);
        QTableView* frozen = report->frozenView();
        QTableView* table = report->tableView();
        QCOMPARE(frozen->model()->index(0, 0).data().toString(), QStringLiteral("Wheel Speed FL max [km/h]"));
        QCOMPARE(frozen->model()->index(1, 0).data().toString(), QStringLiteral("Duty max [%]"));
        QCOMPARE(table->model()->index(1, 1).data().toString(), QStringLiteral("75.000"));
        // The max wheel speed of lap 1 in km/h: the sine's crest 1.3 x 2000 m / 50 s, x 3.6.
        const double maxKmh = table->model()->index(0, 1).data().toString().toDouble();
        QVERIFY(std::abs(maxKmh - 1.3 * 2000.0 / 50.0 * 3.6) < 0.05);
    }

    // (b) M3.md item 26: unit pickers follow the channel's quantity.
    void unitPickersOfferTheChannelQuantity() {
        World w;
        const auto src = withDuty(w);
        selectMain(w, lapRef(*src, 1));
        {
            const project::ComponentSpec spec = specOf("gauge_numeric", R"({"channel":"Duty"})");
            std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *w.context));
            PropertiesDialog dialog(*c);
            QComboBox* unit = combo(dialog, "/displayUnit");
            QComboBox* channel = combo(dialog, "/channel");
            QVERIFY(unit != nullptr && channel != nullptr);
            QVERIFY(unit->isEditable()); // free text (an alias) stays allowed
            QCOMPARE(unitChoices(unit), kRatioUnits);
            channel->setEditText(QStringLiteral("Wheel Speed FL"));
            QCOMPARE(unitChoices(unit), kSpeedUnits);
            channel->setEditText(QStringLiteral("No Such Channel"));
            QCOMPARE(unitChoices(unit), QStringList{QString()});
            channel->setEditText(QStringLiteral("Wheel Speed FL"));
            unit->setCurrentIndex(unit->findText(QStringLiteral("km/h")));
            QVERIFY(dialog.isValid());
            const QJsonObject edited =
                QJsonDocument::fromJson(QByteArray::fromStdString(dialog.editedSpec().body.compact()))
                    .object();
            QCOMPARE(edited.value(QStringLiteral("displayUnit")).toString(), QStringLiteral("km/h"));
            QCOMPARE(edited.value(QStringLiteral("channel")).toString(), QStringLiteral("Wheel Speed FL"));
        }
        {
            std::unique_ptr<ComponentWidget> c(
                ComponentWidget::create(timeDistanceSpec({{"Duty"}, {"Wheel Speed FL"}}), *w.context));
            PropertiesDialog dialog(*c);
            QComboBox* unit = combo(dialog, "/groups/0/traces/0/displayUnit");
            QVERIFY(unit != nullptr);
            QCOMPARE(unitChoices(unit), kRatioUnits);
            QComboBox* second = combo(dialog, "/groups/1/traces/0/displayUnit"); // the second group's record
            QVERIFY(second != nullptr);
            QCOMPARE(unitChoices(second), kSpeedUnits);
        }
        {
            const project::ComponentSpec spec =
                specOf("scatter", R"({"xChannel":"Wheel Speed FL","traces":[{"yChannel":"Duty"}]})");
            std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *w.context));
            PropertiesDialog dialog(*c);
            QCOMPARE(unitChoices(combo(dialog, "/xDisplayUnit")), kSpeedUnits);
            QCOMPARE(unitChoices(combo(dialog, "/traces/0/displayUnit")), kRatioUnits);
            QCOMPARE(unitChoices(combo(dialog, "/colourChannel/displayUnit")), QStringList{QString()});
            combo(dialog, "/colourChannel/channel")->setEditText(QStringLiteral("Duty"));
            QCOMPARE(unitChoices(combo(dialog, "/colourChannel/displayUnit")), kRatioUnits);
        }
        {
            const project::ComponentSpec spec =
                specOf("channel_report", R"({"report":{"channels":[{"name":"Duty","stats":["avg"]}]}})");
            std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *w.context));
            PropertiesDialog dialog(*c);
            QCOMPARE(unitChoices(combo(dialog, "/report/channels/0/displayUnit")), kRatioUnits);
        }
        {
            const project::ComponentSpec spec = valuesListSpec({"Wheel Speed FL"});
            std::unique_ptr<ComponentWidget> c(ComponentWidget::create(spec, *w.context));
            PropertiesDialog dialog(*c);
            QCOMPARE(unitChoices(combo(dialog, "/channels/0/displayUnit")), kSpeedUnits);
        }
    }

    // (c) M3.md item 22: the speed-class column and the bounds in the Corr Dist display unit.
    void trackEditorSpeedClassAndDistanceUnit() {
        EditorWorld e("fu-track");
        TrackEditor editor(e.session, e.w.selection, e.w.maths, e.maps);
        QSignalSpy ready(&editor, &TrackEditor::previewReady);
        editor.generate();
        QVERIFY(QTest::qWaitFor([&] { return ready.count() > 0; }, 30000));
        editor.accept();
        QVERIFY(editor.currentMap() != nullptr);
        QTableWidget& table = *editor.sectionsTable();
        // Without overrides the bounds are in m (Corr Dist is metres), one decimal.
        QCOMPARE(editor.distanceUnit(), QStringLiteral("m"));
        QCOMPARE(columnTitled(table, QStringLiteral("Start (m)")), 2);
        const int speedCol = columnTitled(table, QStringLiteral("Speed class"));
        QVERIFY(speedCol >= 0);
        QCOMPARE(editor.startField()->suffix(), QStringLiteral(" m"));
        QCOMPARE(editor.startField()->decimals(), 1);
        // A section with a speed class; the class is kept by an edit that does not name one.
        const int before = e.session.undoStack().count();
        QVERIFY(editor.addSection(QStringLiteral("T1 entry"), 150.0, 230.0, QStringLiteral("corner"),
                                  QString::fromLatin1(TrackEditor::kUserSet), QStringLiteral("slow")));
        QCOMPARE(e.session.undoStack().count(), before + 1);
        const auto sectionNamed = [&](const char* name) -> std::optional<analysis::TrackSection> {
            for (const analysis::TrackSection& s : editor.currentMap()->sections) {
                if (s.name == name) {
                    return s;
                }
            }
            return std::nullopt;
        };
        const auto speedClassOf = [&](const char* name) {
            const std::optional<analysis::TrackSection> found = sectionNamed(name);
            return found ? QString::fromStdString(found->speedClass) : QStringLiteral("(no such section)");
        };
        std::optional<analysis::TrackSection> s = sectionNamed("T1 entry");
        QVERIFY(s.has_value());
        QCOMPARE(QString::fromStdString(s->speedClass), QStringLiteral("slow"));
        int row = rowNamed(table, QStringLiteral("T1 entry"));
        QVERIFY(row >= 0);
        QCOMPARE(table.item(row, speedCol)->text(), QStringLiteral("slow"));
        const QString id = QString::fromStdString(s->id);
        QVERIFY(editor.editSection(id, QStringLiteral("T1 entry"), 160.0, 240.0, QString()));
        QCOMPARE(speedClassOf("T1 entry"), QStringLiteral("slow"));
        QVERIFY(editor.editSection(id, QStringLiteral("T1 entry"), 160.0, 240.0, QString(),
                                   QStringLiteral("fast")));
        QCOMPARE(speedClassOf("T1 entry"), QStringLiteral("fast"));
        // Only slow, fast or none.
        const int count = e.session.undoStack().count();
        QVERIFY(!editor.addSection(QStringLiteral("Bad"), 300.0, 340.0, QStringLiteral("corner"),
                                   QString::fromLatin1(TrackEditor::kUserSet), QStringLiteral("medium")));
        QVERIFY(!editor.lastError().isEmpty());
        QCOMPARE(e.session.undoStack().count(), count);

        // Corr Speed shown in km/h: Corr Dist follows in km (03 section 9.1 item 6), four decimals.
        setOverrideUnit(e.session, "Corr Speed", "km/h");
        QCOMPARE(editor.distanceUnit(), QStringLiteral("km"));
        QCOMPARE(columnTitled(table, QStringLiteral("Start (km)")), 2);
        QCOMPARE(columnTitled(table, QStringLiteral("End (km)")), 3);
        QCOMPARE(editor.startField()->suffix(), QStringLiteral(" km"));
        QCOMPARE(editor.startField()->decimals(), 4);
        row = rowNamed(table, QStringLiteral("T1 entry"));
        QCOMPARE(table.item(row, 2)->text(), QStringLiteral("0.1600"));
        QCOMPARE(table.item(row, 3)->text(), QStringLiteral("0.2400"));
        table.setCurrentCell(row, 0);
        QCOMPARE(editor.startField()->value(), 0.16);
        QCOMPARE(editor.endField()->value(), 0.24);
        QCOMPARE(editor.speedClassField()->currentData().toString(), QStringLiteral("fast"));
        // Editing in km stores metres.
        editor.startField()->setValue(0.17);
        QPushButton* apply = editor.findChild<QPushButton*>(QStringLiteral("trackEditorApply"));
        QVERIFY(apply != nullptr);
        apply->click();
        s = sectionNamed("T1 entry");
        QVERIFY(s.has_value());
        QVERIFY(std::abs(s->startM - 170.0) < 1e-6);
        QCOMPARE(speedClassOf("T1 entry"), QStringLiteral("fast"));
        // The Corr Dist override wins over the speed rule.
        setOverrideUnit(e.session, "Corr Dist", "mi");
        QCOMPARE(editor.distanceUnit(), QStringLiteral("mi"));
        QCOMPARE(editor.startField()->decimals(), 5);
    }

    // (d) M3.md item 24: Export Data in the legacy dialect, values in the display unit.
    void exportWritesTheLegacyDialectInDisplayUnits() {
        World w;
        const auto src = withDuty(w);
        selectMain(w, lapRef(*src, 1));
        Rig<NumericGauge> rig(w, specOf("gauge_numeric", R"({"channel":"Duty"})"), QSize(240, 160));
        QVERIFY(waitQuiet(*rig.c));
        MemorySink sink;
        QVERIFY(rig.c->exportData(sink, ExportOptions{}));
        QVERIFY(legacycsv::crlfOnly(sink.bytes));
        const QList<QByteArray> lines = legacycsv::lines(sink.bytes);
        QCOMPARE(lines.size(), legacycsv::kLinesBeforeData + 50 * 50);
        for (int i = 0; i < legacycsv::kMetadataLines; ++i) {
            QVERIFY(!lines[i].isEmpty());
        }
        QVERIFY(lines[legacycsv::kMetadataLines].isEmpty() && lines[legacycsv::kMetadataLines + 1].isEmpty());
        QVERIFY(lines[legacycsv::kUnitsRow + 1].isEmpty() && lines[legacycsv::kUnitsRow + 2].isEmpty());
        // The range and the worksheet the export comes from.
        const QList<QByteArray> range = legacycsv::fields(lines[10]);
        QCOMPARE(range.value(0), QByteArray("Range"));
        QCOMPARE(range.value(1), QByteArray::fromStdString(lapAt(*src, 1).name));
        const QList<QByteArray> sheet = legacycsv::fields(lines[1]);
        QCOMPARE(sheet.value(4), QByteArray("Worksheet"));
        QCOMPARE(sheet.value(5), QByteArray::fromStdString(w.sheet.name));
        // The rate and the span (50 Hz, 50 s).
        QCOMPARE(legacycsv::fields(lines[8]).value(1).toDouble(), 50.0);
        QCOMPARE(legacycsv::fields(lines[9]).value(1).toDouble(), 50.0);
        // Duty in % (its default display unit): 25 at even samples, 75 at odd ones.
        QCOMPARE(lines[legacycsv::kNamesRow], QByteArray("\"Time\",\"Duty\""));
        QCOMPARE(lines[legacycsv::kUnitsRow], QByteArray("\"s\",\"%\""));
        for (int k = 0; k < 4; ++k) {
            const QList<QByteArray> f = legacycsv::fields(lines[legacycsv::kFirstDataRow + k]);
            QVERIFY(legacycsv::allQuoted(lines[legacycsv::kFirstDataRow + k]));
            QCOMPARE(f[0].toDouble(), 0.02 * k);
            QCOMPARE(f[1].toDouble(), k % 2 == 0 ? 25.0 : 75.0);
        }
        // A named display unit: the wheel speed in km/h.
        Rig<NumericGauge> speed(
            w, specOf("gauge_numeric", R"({"channel":"Wheel Speed FL","displayUnit":"km/h"})"),
            QSize(240, 160));
        QVERIFY(waitQuiet(*speed.c));
        MemorySink kmh;
        QVERIFY(speed.c->exportData(kmh, ExportOptions{}));
        const QList<QByteArray> k = legacycsv::lines(kmh.bytes);
        QCOMPARE(k[legacycsv::kUnitsRow], QByteArray("\"s\",\"km/h\""));
        const SourceShape shape;
        const double first = legacycsv::fields(k[legacycsv::kFirstDataRow]).value(1).toDouble();
        QVERIFY(std::abs(first - static_cast<double>(static_cast<float>(shape.wheelFl(2000))) * 3.6) < 1e-4);
    }
};

QTEST_MAIN(TstFollowupsUnits)
#include "tst_followups_units.moc"
