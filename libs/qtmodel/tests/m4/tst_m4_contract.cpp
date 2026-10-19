// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The final parts of the esses_qtmodel M4 contract (D-206, docs/milestones/M4.md section 2): the budget
// setting's key and floor, OpenBudgetCheck::exceeds, PerformanceMonitor's sampling, timer and signal, the
// SourceLoader M4 signals and its budget gate pointer, the alternate action of a Messages row, and the Data
// window model's stale query and role. Every M4 member is implemented (D-216: no contract stub is left); the
// behaviour is tested by the packages that built it (budget D-207, reload D-208, followups D-210).

#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QMetaMethod>
#include <QSignalSpy>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::qt;

Q_DECLARE_METATYPE(esses::qt::PerformanceSnapshot)

namespace {

AppRuntimeOptions options() {
    AppRuntimeOptions o;
    o.workers = 2;
    o.cacheBudget = std::size_t{96} << 20;
    return o;
}

bool hasSignal(const QMetaObject& meta, const char* signature) {
    const int i = meta.indexOfSignal(QMetaObject::normalizedSignature(signature).constData());
    return i >= 0 && meta.method(i).methodType() == QMetaMethod::Signal;
}

} // namespace

class TstM4Contract final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void budgetSettingKeyAndFloor() {
        QCOMPARE(QString::fromLatin1(kBudgetSettingKey), QStringLiteral("memory.chunkCacheBudgetBytes"));
        QCOMPARE(kMinBudgetBytes, std::size_t{1073741824});
    }

    void openBudgetCheckComparesTheEstimateWithTheBudget() {
        OpenBudgetCheck c;
        c.budgetBytes = 1000;
        c.residentBytes = 900; // reported, never added (07 section 7.1)
        c.estimateBytes = 1000;
        QVERIFY(!c.exceeds());
        c.estimateBytes = 1001;
        QVERIFY(c.exceeds());
        c.estimateBytes = 0;
        QVERIFY(!c.exceeds());
    }

    void performanceMonitorSamplesTheRuntime() {
        AppRuntime rt(options());
        PerformanceMonitor monitor(rt);
        QCOMPARE(monitor.interval(), PerformanceMonitor::kDefaultIntervalMs);
        monitor.setInterval(5);
        QCOMPARE(monitor.interval(), 100);
        monitor.setInterval(250);
        QCOMPARE(monitor.interval(), 250);

        const PerformanceSnapshot s = monitor.sample();
        QCOMPARE(s.budgetBytes, std::size_t{96} << 20);
        QCOMPARE(s.cache.residentBytes, rt.cache().stats().residentBytes);
        QCOMPARE(s.pool.workers, std::size_t{2});

        QSignalSpy spy(&monitor, &PerformanceMonitor::updated);
        QVERIFY(spy.isValid());
        monitor.refresh();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(monitor.last().budgetBytes, std::size_t{96} << 20);
        QCOMPARE(spy.at(0).at(0).value<PerformanceSnapshot>().pool.workers, std::size_t{2});

        QVERIFY(!monitor.running());
        monitor.setInterval(100);
        monitor.start();
        QVERIFY(monitor.running());
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 2, 20000); // a timer tick, however late it comes
        monitor.stop();
        QVERIFY(!monitor.running());

        rt.shutdown();
        QCOMPARE(monitor.sample().budgetBytes, std::size_t{0}); // nothing is read after shutdown
    }

    void sourceLoaderHasTheM4SignalsAndTheGate() {
        const QMetaObject& meta = SourceLoader::staticMetaObject;
        QVERIFY(hasSignal(meta, "budgetExceeded(int,esses::qt::OpenBudgetCheck)"));
        QVERIFY(hasSignal(meta, "staleChanged(esses::model::SourceId,bool)"));
        QVERIFY(hasSignal(
            meta, "sourceReloaded(esses::model::SourceId,std::shared_ptr<const esses::model::DataSource>)"));
        QVERIFY(hasSignal(meta, "summariesReady(esses::model::SourceId)"));

        AppRuntime rt(options());
        {
            SourceLoader loader(rt.pool(), rt.dispatcher(), nullptr);
            QVERIFY(loader.budgetGate() == nullptr); // off by default: headless opens never wait
            loader.setBudgetGate(&rt.cache());
            QVERIFY(loader.budgetGate() == &rt.cache());
            loader.setBudgetGate(nullptr);
            QVERIFY(loader.budgetGate() == nullptr);
        }
        rt.shutdown();
    }

    void messagesRowsCarryAnAlternateAction() {
        MessagesModel model;
        Message m;
        m.text = QStringLiteral("big.ld: above the budget");
        m.action = QStringLiteral("file.continue_open");
        m.actionArgs = {7};
        m.altAction = QStringLiteral("file.cancel_open");
        m.altActionArgs = {7};
        model.append(m);
        const QModelIndex row = model.index(0);
        QCOMPARE(model.data(row, MessagesModel::ActionRole).toString(), QStringLiteral("file.continue_open"));
        QCOMPARE(model.data(row, MessagesModel::AltActionRole).toString(),
                 QStringLiteral("file.cancel_open"));
        QCOMPARE(model.at(0).altActionArgs, QVariantList{7});
        QCOMPARE(model.roleNames().value(MessagesModel::AltActionRole), QByteArray("altAction"));
        QCOMPARE(static_cast<int>(MessagesModel::AltActionRole),
                 static_cast<int>(MessagesModel::TimestampRole) + 1);
    }

    void dataWindowModelAsksTheStaleQuery() {
        QCOMPARE(static_cast<int>(DataWindowModel::StaleRole),
                 static_cast<int>(DataWindowModel::FilteredInRole) + 1);
        DataSelectionModel selection;
        DataWindowModel model(selection);
        model::SourceId a;
        a.lo = 1;
        model::SourceId b;
        b.lo = 2;
        QVERIFY(!model.isStale(a)); // no query: nothing is stale
        model.setStaleQuery([a](const model::SourceId& id) { return id == a; });
        QVERIFY(model.isStale(a));
        QVERIFY(!model.isStale(b));
        model.setStaleQuery({});
        QVERIFY(!model.isStale(a));
    }
};

QTEST_GUILESS_MAIN(TstM4Contract)
#include "tst_m4_contract.moc"
