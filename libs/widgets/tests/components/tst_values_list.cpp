// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesList (04 section 15), offscreen: rows from the body's channels, the columns/heading/width options,
// values at the cursor for Main and the reference overlay (04 section 3.2) following the worksheet,
// addChannel through specEditRequested, applySpec, the scheme fonts and renderToImage.

#include "components/component_test_support.hpp"

#include <esses/qtmodel/values_model.hpp>
#include <esses/widgets/components/values_list.hpp>

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::cmptest;

namespace {

constexpr Micros kSec = 1'000'000;

project::ComponentSpec withBody(project::ComponentSpec spec, const QJsonObject& body) {
    Result<model::OpaqueJson> json =
        model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    spec.body = std::move(json).value();
    return spec;
}

} // namespace

class TstValuesList : public QObject {
    Q_OBJECT
public:
    static void initMain() { prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        registerGoldenFonts();
        registerBuiltinComponents();
        QVERIFY(ComponentWidget::isRegistered(QString::fromLatin1(ValuesList::kType)));
    }

    void rowsColumnsAndValues() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        w.selection.toggleOverlay(lapRef(*a, 2));
        const project::ComponentSpec spec = valuesListSpec({"Throttle", "Wheel Speed FL"});
        std::unique_ptr<ComponentWidget> owner(ComponentWidget::create(spec, *w.context));
        auto* list = qobject_cast<ValuesList*>(owner.get());
        QVERIFY(list != nullptr);
        list->resize(320, 200);
        list->show();
        QCOMPARE(list->overlayPolicy(), ComponentWidget::OverlayPolicy::MainAndReference);
        QCOMPARE(list->channelNames(),
                 (QStringList{QStringLiteral("Throttle"), QStringLiteral("Wheel Speed FL")}));
        qt::ValuesModel& m = list->model();
        QCOMPARE(m.rowCount(), 2);
        QVERIFY(!list->table().isColumnHidden(qt::ValuesModel::DeltaCol));
        QVERIFY(!list->table().isColumnHidden(qt::ValuesModel::UnitCol));
        QVERIFY(list->table().horizontalHeader()->isVisible());
        QVERIFY(!list->table().tabKeyNavigation());

        // Values at the cursor for Main and the reference overlay; they follow the cursor.
        w.view->setCursor(0, qt::CursorSource::Programmatic);
        QTRY_VERIFY(!m.cellText(0, qt::ValuesModel::MainCol).isEmpty() &&
                    m.cellText(0, qt::ValuesModel::MainCol) != QStringLiteral("…"));
        QCOMPARE(m.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("100.00")); // throttle at lap start
        QTRY_COMPARE(m.cellText(0, qt::ValuesModel::ReferenceCol), QStringLiteral("100.00"));
        const QString atStart = m.cellText(0, qt::ValuesModel::MainCol);
        w.view->setCursor(25 * kSec, qt::CursorSource::Programmatic); // half of the 50 s lap: throttle 0
        QTRY_COMPARE(m.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("0.00"));
        QVERIFY(m.cellText(0, qt::ValuesModel::MainCol) != atStart);

        // Columns, heading and widths from the body.
        QJsonObject body;
        body.insert(QStringLiteral("channels"),
                    QJsonArray{QJsonObject{{QStringLiteral("channel"), QStringLiteral("Throttle")},
                                           {QStringLiteral("dps"), 0}}});
        body.insert(QStringLiteral("columns"), QJsonObject{{QStringLiteral("datum"), false},
                                                           {QStringLiteral("delta"), false},
                                                           {QStringLiteral("inlineUnits"), false}});
        body.insert(QStringLiteral("showHeading"), false);
        body.insert(QStringLiteral("autoSizeColumns"), false);
        body.insert(QStringLiteral("widths"),
                    QJsonObject{{QStringLiteral("name"), 140}, {QStringLiteral("value"), 70}});
        list->applySpec(withBody(spec, body));
        QCOMPARE(m.rowCount(), 1);
        QVERIFY(list->table().isColumnHidden(qt::ValuesModel::DeltaCol));
        QVERIFY(list->table().isColumnHidden(qt::ValuesModel::UnitCol));
        QVERIFY(list->table().horizontalHeader()->isHidden());
        QCOMPARE(list->table().columnWidth(qt::ValuesModel::NameCol), 140);
        QCOMPARE(list->table().columnWidth(qt::ValuesModel::MainCol), 70);
        QTRY_COMPARE(m.cellText(0, qt::ValuesModel::MainCol), QStringLiteral("0")); // dps 0

        const QImage img = list->renderToImage(list->devicePixelRatioF());
        QCOMPARE(img.size(), (QSizeF(320, 200) * list->devicePixelRatioF()).toSize());
    }

    void addChannelEmitsAnEdit() {
        World w;
        const auto a = w.publish(SourceOptions{});
        w.selection.setMain(lapRef(*a, 1));
        ValuesList list(valuesListSpec({"Throttle"}), *w.context);
        QSignalSpy edits(&list, &ComponentWidget::specEditRequested);
        list.addChannel(QStringLiteral("Throttle"), false); // already a row
        QCOMPARE(edits.size(), 0);
        list.addChannel(QStringLiteral("Corr Speed"), true);
        QCOMPARE(edits.size(), 1);
        const project::ComponentSpec next = edits.constLast().at(0).value<project::ComponentSpec>();
        QCOMPARE(list.spec().body,
                 valuesListSpec({"Throttle"}, {}, 0, list.spec().id.str()).body); // unchanged
        const QJsonObject body =
            QJsonDocument::fromJson(QByteArray::fromStdString(next.body.compact())).object();
        QCOMPARE(body.value(QStringLiteral("channels")).toArray().size(), 2);
        QCOMPARE(body.value(QStringLiteral("columns")).toObject().value(QStringLiteral("delta")).toBool(),
                 true);
        list.applySpec(next);
        QCOMPARE(list.channelNames(),
                 (QStringList{QStringLiteral("Throttle"), QStringLiteral("Corr Speed")}));
    }

    void schemeFonts() {
        World w;
        ValuesList list(valuesListSpec({"Throttle"}), *w.context);
        QCOMPARE(list.table().font().family(), QString::fromStdString(w.scheme->font("valuesItem").family));
        QCOMPARE(list.table().font().pointSizeF(), w.scheme->font("valuesItem").pointSize);
        project::Scheme bigger = *w.scheme;
        bigger.fonts["valuesItem"].pointSize = 15.0;
        list.onSchemeChanged(bigger);
        QCOMPARE(list.table().font().pointSizeF(), 15.0);
    }
};

QTEST_MAIN(TstValuesList)
#include "tst_values_list.moc"
