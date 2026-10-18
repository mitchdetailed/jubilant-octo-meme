// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeedDialog (06 section 13.6, 03 section 9.1, D-016): it shows project.json
// builtinMaths["esses.corrected_speed_distance"] (the defaults when absent) and options.lapStretch; Apply
// writes the edited settings, the module's enabled flag and the lap-stretch options as one undo command,
// the written settings validate against schemas/builtin-esses.corrected_speed_distance-1.schema.json and
// parse into the CorrectedSpeedSettings the maths module runs with (MathsService gets them through
// ProjectSession::documentsChanged), an untouched dialog writes nothing, undo brings the page back, invalid
// input (a G-correction recovery below 0, a tolerance that is not a number) disables Apply with the reason;
// setSettings() of a CorrectedSpeedSettings is written by the next Apply.

#include "properties_shell/shell_test_support.hpp"

#include <esses/project/schema_registry.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/field_binder.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QUndoStack>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

struct Project {
    QTemporaryDir config;
    QTemporaryDir projects;
    std::unique_ptr<qt::ProjectSession> session;
    bool start() {
        if (!config.isValid() || !projects.isValid()) {
            return false;
        }
        const Paths paths = pathsFor(config.path());
        const std::filesystem::path folder = fsPath(projects.path()) / "speed";
        if (!makeProject(paths, folder, "Speed")) {
            return false;
        }
        session = std::make_unique<qt::ProjectSession>(paths);
        return session->loadDefaults().ok() && session->openProject(folder).ok();
    }
    QJsonObject settings() const { return jsonptr::fromDoc(session->correctedSpeedSettings()).toObject(); }
};

} // namespace

class TstCorrectedSpeedDialog : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void applyIsOneUndoableEdit() {
        Project p;
        QVERIFY(p.start());
        cmptest::World world;
        CorrectedSpeedDialog dialog(*p.session, world.maths);
        QUndoStack& stack = p.session->undoStack();
        QVERIFY(dialog.validationMessage().isEmpty());
        // Untouched: nothing to write.
        dialog.apply();
        QCOMPARE(stack.count(), 0);
        // Method, the alternate list's first channel, a minimum clamp that invalidates, the stretch
        // tolerance.
        auto* method = qobject_cast<QComboBox*>(dialog.binder().editorFor(QStringLiteral("/method")));
        QVERIFY(method != nullptr);
        method->setCurrentIndex(method->findText(QStringLiteral("gCorrection")));
        auto* speed =
            qobject_cast<QComboBox*>(dialog.binder().editorFor(QStringLiteral("/gCorrection/speedChannel")));
        speed->setEditText(QStringLiteral("Wheel Speed RL"));
        auto* minV = qobject_cast<NumberEdit*>(dialog.binder().editorFor(QStringLiteral("/clamp/min/v")));
        QVERIFY(minV != nullptr); // the optional limit is switched on
        minV->setText(QStringLiteral("5"));
        qobject_cast<QCheckBox*>(dialog.binder().editorFor(QStringLiteral("/clamp/minInvalidate")))
            ->setChecked(true);
        dialog.toleranceEdit().setText(QStringLiteral("3"));
        QVERIFY2(dialog.validationMessage().isEmpty(), qPrintable(dialog.validationMessage()));
        dialog.apply();
        QCOMPARE(stack.count(), 1); // one command for the settings and the stretch options
        const QJsonObject s = p.settings();
        QCOMPARE(s.value(QStringLiteral("method")).toString(), QStringLiteral("gCorrection"));
        QCOMPARE(jsonptr::get(s, QStringLiteral("/gCorrection/speedChannel")).toString(),
                 QStringLiteral("Wheel Speed RL"));
        QCOMPARE(jsonptr::get(s, QStringLiteral("/clamp/min/v")).toDouble(), 5.0);
        QCOMPARE(jsonptr::get(s, QStringLiteral("/clamp/minInvalidate")).toBool(), true);
        QVERIFY(project::SchemaRegistry::instance()
                    .validate(p.session->correctedSpeedSettings(), "builtin-esses.corrected_speed_distance-1")
                    .ok());
        QCOMPARE(p.session->lapStretchSettings().tolerance, 0.03);
        // The maths module runs with what was written.
        const Result<analysis::CorrectedSpeedSettings> parsed = analysis::CorrectedSpeedSettings::fromJson(
            p.session->correctedSpeedSettings(), *p.session->units());
        QVERIFY(parsed.ok());
        QVERIFY(parsed->method == analysis::SpeedMethod::GCorrection);
        QCOMPARE(parsed->gCorrection.speedChannel, std::string("Wheel Speed RL"));
        QVERIFY(dialog.settings().method == analysis::SpeedMethod::GCorrection);
        // Undo: project.json and the page are back.
        stack.undo();
        QVERIFY(p.settings().value(QStringLiteral("method")).toString() != QStringLiteral("gCorrection"));
        QVERIFY(method->currentText() != QStringLiteral("gCorrection"));
        QCOMPARE(p.session->lapStretchSettings().tolerance, 0.05);
        // The module switched off.
        dialog.enabledBox().setChecked(false);
        dialog.apply();
        QVERIFY(!p.session->correctedSpeedEnabled());
    }

    void validationAndSetSettings() {
        Project p;
        QVERIFY(p.start());
        cmptest::World world;
        CorrectedSpeedDialog dialog(*p.session, world.maths);
        auto* recover = qobject_cast<NumberEdit*>(
            dialog.binder().editorFor(QStringLiteral("/gCorrection/recoverSeconds")));
        recover->setText(QStringLiteral("-1"));
        QVERIFY(dialog.validationMessage().contains(QStringLiteral("at least 0")));
        dialog.apply();
        QCOMPARE(p.session->undoStack().count(), 0);
        recover->setText(QStringLiteral("0.25"));
        dialog.toleranceEdit().setText(QStringLiteral("abc"));
        QVERIFY(!dialog.validationMessage().isEmpty());
        dialog.toleranceEdit().setText(QStringLiteral("5"));
        QVERIFY(dialog.validationMessage().isEmpty());
        // setSettings: the next Apply writes them.
        analysis::CorrectedSpeedSettings s;
        s.method = analysis::SpeedMethod::AlternateList;
        s.alternateListChannels = {"GPS Speed", "Ground Speed"};
        s.clamp.maxMps = 100.0;
        dialog.setSettings(s);
        QVERIFY(dialog.settings().method == analysis::SpeedMethod::AlternateList);
        dialog.apply();
        const QJsonObject written = p.settings();
        QCOMPARE(written.value(QStringLiteral("method")).toString(), QStringLiteral("alternateList"));
        QCOMPARE(jsonptr::get(written, QStringLiteral("/alternateList/channels/0")).toString(),
                 QStringLiteral("GPS Speed"));
        QCOMPARE(jsonptr::get(written, QStringLiteral("/clamp/max/v")).toDouble(), 360.0); // km/h
        QVERIFY(project::SchemaRegistry::instance()
                    .validate(p.session->correctedSpeedSettings(), "builtin-esses.corrected_speed_distance-1")
                    .ok());
    }
};

QTEST_MAIN(TstCorrectedSpeedDialog)
#include "tst_corrected_speed_dialog.moc"
