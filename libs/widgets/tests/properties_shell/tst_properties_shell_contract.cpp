// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 contract of the properties/shell work (07 section 6.1; 06 sections 4.2, 4.3, 4.5, 12, 13.5, 13.6,
// 15): every class constructs offscreen over the qtmodel services it is declared against, and the final parts
// behave: FieldBinder's binding table (a Custom row binds its subtree, AC-M3-08's coverage rule),
// AnimationController's rate list and state signals, the clipboard MIME type, a PropertiesDialog's binder and
// component. Behaviour tests of the M3-STUB members belong to their owners.

#include "components/component_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/properties_shell/component_clipboard.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>

#include <QCheckBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

// A ProjectSession over the shipped defaults in a temporary configuration directory.
struct Session {
    QTemporaryDir config;
    std::unique_ptr<qt::ProjectSession> session;

    bool start() {
        if (!config.isValid()) {
            return false;
        }
        PathOverrides po;
        po.configDir = std::filesystem::path(config.path().toStdWString());
        po.dataDir = repoRoot() / "data";
        auto paths = Paths::resolve(po);
        if (!paths) {
            return false;
        }
        session = std::make_unique<qt::ProjectSession>(*paths);
        return session->loadDefaults().ok();
    }
};

} // namespace

class TstPropertiesShellContract : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void fieldBinderTable() {
        FieldBinder binder;
        QLineEdit name;
        QCheckBox grid;
        QWidget groups;
        binder.bind(QStringLiteral("/display/showGrid"), FieldKind::Bool, &grid, QStringLiteral("Grid"));
        binder.bind(QStringLiteral("/groups"), FieldKind::Custom, &groups, QStringLiteral("Groups"));
        binder.bind(QStringLiteral("/display/bandGroup"), FieldKind::Text, &name,
                    QStringLiteral("Band group"));
        QCOMPARE(binder.bindings().size(), std::size_t{3});
        QCOMPARE(binder.boundPointers(),
                 (QStringList{QStringLiteral("/display/showGrid"), QStringLiteral("/groups"),
                              QStringLiteral("/display/bandGroup")}));
        QVERIFY(binder.isBound(QStringLiteral("/display/showGrid")));
        QVERIFY(binder.isBound(QStringLiteral("/groups/0/traces/0/channel"))); // inside the Custom subtree
        QVERIFY(!binder.isBound(QStringLiteral("/display/showGridLines")));    // a prefix is not an ancestor
        QVERIFY(!binder.isBound(QStringLiteral("/display")));
        QCOMPARE(binder.bindings()[1].kind, FieldKind::Custom);
        QCOMPARE(binder.bindings()[0].editor.data(), static_cast<QWidget*>(&grid));
    }

    void propertiesDialogTracksItsComponent() {
        World w;
        std::unique_ptr<ComponentWidget> c(ComponentWidget::create(valuesListSpec({"Throttle"}), *w.context));
        QVERIFY(c != nullptr);
        PropertiesDialog dialog(*c);
        QCOMPARE(&dialog.component(), c.get());
        QCOMPARE(&dialog.binder(), &dialog.binder());
        QVERIFY(!dialog.isModal());
    }

    void animationControllerState() {
        QCOMPARE(AnimationController::kRates.size(), std::size_t{5});
        QCOMPARE(AnimationController::kRates.front(), 0.25);
        QCOMPARE(AnimationController::kRates.back(), 4.0);
        AnimationController a;
        QSignalSpy on(&a, &AnimationController::animatingChanged);
        QSignalSpy rate(&a, &AnimationController::rateChanged);
        QVERIFY(!a.animating());
        QCOMPARE(a.rate(), 1.0);
        a.toggle();
        QVERIFY(a.animating());
        a.toggle();
        QVERIFY(!a.animating());
        QCOMPARE(on.count(), 2);
        a.setRate(2.0);
        a.setRate(2.0);
        a.setRate(0.0); // ignored
        QCOMPARE(a.rate(), 2.0);
        QCOMPARE(rate.count(), 1);
    }

    void clipboardMimeType() {
        QCOMPARE(QString::fromLatin1(ComponentClipboard::kMimeType),
                 QStringLiteral("application/x-esses-components+json"));
        ComponentClipboard::Payload p;
        QCOMPARE(p.schemaVersion, 1);
        QCOMPARE(p.sourceDomain, QStringLiteral("time"));
    }

    void shellWidgetsConstruct() {
        World w;
        ZoomSummaryBar bar;
        bar.setWorksheet(w.view.get());
        QCOMPARE(bar.worksheet(), w.view.get());
        bar.setSelection(&w.selection);
        QVERIFY(bar.sizeHint().height() > 0);
        CommandPalette palette(w.commands, nullptr, nullptr);
        palette.setQuery(QStringLiteral("zoom"));

        Session s;
        QVERIFY(s.start());
        LayoutEditor layout(*s.session);
        CorrectedSpeedDialog speed(*s.session, w.maths);
        analysis::CorrectedSpeedSettings settings;
        settings.method = analysis::SpeedMethod::AlternateList;
        speed.setSettings(settings);
        QVERIFY(speed.settings().method == analysis::SpeedMethod::AlternateList);
        qt::TrackMapService maps(s.session.get(), &w.maths, &w.rt.dispatcher());
        TrackEditor track(*s.session, w.selection, w.maths, maps);
        QVERIFY(!track.isModal());
    }
};

QTEST_MAIN(TstPropertiesShellContract)
#include "tst_properties_shell_contract.moc"
