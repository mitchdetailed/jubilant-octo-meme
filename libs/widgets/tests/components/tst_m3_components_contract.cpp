// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 contract of the components (07 section 6.1; 04 sections 7.4, 9-17): every MVP type is registered and
// created by its own class (never the placeholder), with the overlay policy of 04 section 7.4 and the key
// context of 06 section 14.2; the Track Map dock and the Track Editor construct. Behaviour tests belong to
// the owners of the classes (tests/components/tst_<class>.cpp).

#include "components/component_test_support.hpp"

#include <esses/widgets/components/bar_gauge.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/histogram.hpp>
#include <esses/widgets/components/numeric_gauge.hpp>
#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/components/placeholder_component.hpp>
#include <esses/widgets/components/scatter_plot.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/components/values_list.hpp>

#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

project::ComponentSpec specOf(const char* type) {
    project::ComponentSpec spec;
    spec.id = Uuid::parse("6f1c1f9e-1a51-4f5a-9d43-0a7d2d5f0301");
    spec.type = type;
    return spec;
}

} // namespace

class TstM3ComponentsContract : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
    }

    void everyMvpTypeIsRegistered_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("policy");
        QTest::addColumn<int>("context");
        using P = ComponentWidget::OverlayPolicy;
        using C = qt::CommandContext;
        const auto row = [](const char* type, P policy, C context) {
            QTest::newRow(type) << QString::fromLatin1(type) << static_cast<int>(policy)
                                << static_cast<int>(context);
        };
        row(TimeDistanceGraph::kType, P::AllOverlays, C::Graph);
        row(OutingGraph::kType, P::MainOnly, C::Graph);
        row(ScatterPlot::kType, P::AllOverlays, C::Summary);
        row(Histogram::kType, P::AllOverlays, C::Summary);
        row(ChannelReport::kType, P::AllOverlays, C::Component);
        row(TimeReport::kType, P::AllOverlays, C::Component);
        row(TrackPosition::kType, P::AllOverlays, C::Track);
        row(ValuesList::kType, P::MainAndReference, C::Component);
        row(NumericGauge::kType, P::MainAndReference, C::Component);
        row(BarGauge::kType, P::MainAndReference, C::Component);
    }
    void everyMvpTypeIsRegistered() {
        QFETCH(QString, type);
        QFETCH(int, policy);
        QFETCH(int, context);
        QVERIFY(ComponentWidget::isRegistered(type));
        World w;
        std::unique_ptr<ComponentWidget> c(
            ComponentWidget::create(specOf(type.toLatin1().constData()), *w.context));
        QVERIFY(c != nullptr);
        QVERIFY(qobject_cast<PlaceholderComponent*>(c.get()) == nullptr);
        QCOMPARE(QString::fromLatin1(c->type()), type);
        QCOMPARE(static_cast<int>(c->overlayPolicy()), policy);
        QCOMPARE(static_cast<int>(c->commandContext()), context);
    }

    void aLaterTypeStillRendersThePlaceholder() {
        World w;
        std::unique_ptr<ComponentWidget> c(ComponentWidget::create(specOf("gauge_dial"), *w.context));
        QVERIFY(qobject_cast<PlaceholderComponent*>(c.get()) != nullptr);
    }

    void trackMapWindowFollowsAContext() {
        World w;
        TrackMapWindow dock;
        QVERIFY(dock.context() == nullptr);
        dock.setContext(w.context.get());
        QCOMPARE(dock.context(), w.context.get());
        dock.setContext(nullptr);
        QVERIFY(dock.context() == nullptr);
    }
};

QTEST_MAIN(TstM3ComponentsContract)
#include "tst_m3_components_contract.moc"
