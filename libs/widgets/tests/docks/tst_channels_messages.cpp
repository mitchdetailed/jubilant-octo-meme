// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsWindow (06 section 7; the M2 search itself is tst_channels_search) and MessagesWindow (06 section
// 11), offscreen: the filter keeps the source/category rows of a match and highlights the first match, ENTER
// / CTRL+ENTER / double-click request the channel for the focused component, DOWN moves into the list, the
// drag payload (application/x-esses-channels) and its parser; the Messages severity buttons with counts, the
// text filter, Copy/Save As text, action rows (ENTER and double-click run the command), Clear.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <functional>
#include <memory>

// The qtmodel synthetic-source helpers (in memory, circuit laps), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

QModelIndex findChannel(const QAbstractItemModel& m, const QString& name, const QModelIndex& parent = {}) {
    for (int r = 0; r < m.rowCount(parent); ++r) {
        const QModelIndex i = m.index(r, 0, parent);
        if (i.data(qt::ChannelsModel::ChannelNameRole).toString() == name) {
            return i;
        }
        if (const QModelIndex found = findChannel(m, name, i); found.isValid()) {
            return found;
        }
    }
    return {};
}

qt::Message message(qt::Message::Severity s, qt::Message::Origin o, const QString& source,
                    const QString& text) {
    qt::Message m;
    m.severity = s;
    m.origin = o;
    m.source = source;
    m.text = text;
    return m;
}

} // namespace

class TstChannelsMessages : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void channelsFilterAndRequests() {
        qt::AppRuntimeOptions o;
        o.workers = 2;
        o.cacheBudget = std::size_t{64} << 20;
        o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
        qt::AppRuntime rt(o);
        {
            qt::MathsService maths(rt.pool(), rt.dispatcher(), qt::test::shippedUnits());
            qt::DataSelectionModel selection;
            qt::CommandRegistry commands;
            qt::test::SyntheticOptions so;
            so.path = "channels.ld";
            const auto src = qt::test::makeSource(so, rt.cache());
            maths.addSource(src);
            selection.sourcePublished(src);
            qt::ChannelsModel model(selection, maths);
            model.rebuild();
            ChannelsWindow win(model, commands);
            win.resize(300, 400);
            win.show();
            QVERIFY(QTest::qWaitForWindowExposed(&win));
            win.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&win));
            win.search().setFocus();

            const QStringList all = win.visibleChannels();
            for (const char* name :
                 {"Throttle", "Wheel Speed FL", "Wheel Speed FR", "Corr Speed", "Corr Dist"}) {
                QVERIFY2(all.contains(QString::fromLatin1(name)), name);
            }
            // Word prefixes (06 section 7 item 3): "wheel" and "fl" start two name words, in order.
            QTest::keyClicks(&win.search(), QStringLiteral("wheel fl"));
            QCOMPARE(win.visibleChannels(), QStringList{QStringLiteral("Wheel Speed FL")});
            QCOMPARE(win.filterModel().rowCount(), 1); // the source row stays
            QCOMPARE(win.currentChannel(),
                     QStringLiteral("Wheel Speed FL")); // the first match is highlighted
            QSignalSpy requested(&win, &ChannelsWindow::addChannelRequested);
            QTest::keyClick(&win.search(), Qt::Key_Return);
            QCOMPARE(requested.count(), 1);
            QCOMPARE(requested.last().at(0).toString(), QStringLiteral("Wheel Speed FL"));
            QCOMPARE(requested.last().at(1).toBool(), false);
            QTest::keyClick(&win.search(), Qt::Key_Return, Qt::ControlModifier); // as a new panel
            QCOMPARE(requested.count(), 2);
            QCOMPARE(requested.last().at(1).toBool(), true);

            win.search().clear();
            QTest::keyClicks(&win.search(), QStringLiteral("SPEED"));
            QCOMPARE(win.visibleChannels().size(), 3);    // Wheel Speed FL/FR, Corr Speed
            QTest::keyClick(&win.search(), Qt::Key_Down); // into the list on the first match
            QVERIFY(QApplication::focusWidget() == &win.tree());
            QTest::keyClick(&win.tree(), Qt::Key_Down);
            const QString second = win.currentChannel();
            QVERIFY(!second.isEmpty());
            QTest::keyClick(&win.tree(), Qt::Key_Enter, Qt::KeypadModifier);
            QCOMPARE(requested.last().at(0).toString(), second);
            Q_EMIT win.tree().doubleClicked(findChannel(win.filterModel(), QStringLiteral("Corr Speed")));
            QCOMPARE(requested.last().at(0).toString(), QStringLiteral("Corr Speed"));
            QCOMPARE(win.search().text(), QStringLiteral("SPEED")); // kept

            // Drag: channel rows only, MIME application/x-esses-channels.
            const QModelIndex fl = findChannel(win.filterModel(), QStringLiteral("Wheel Speed FL"));
            QVERIFY(fl.isValid());
            QVERIFY(win.filterModel().flags(fl).testFlag(Qt::ItemIsDragEnabled));
            QVERIFY(!win.filterModel().flags(fl.parent()).testFlag(Qt::ItemIsDragEnabled));
            std::unique_ptr<QMimeData> mime(win.filterModel().mimeData({fl}));
            QVERIFY(mime != nullptr);
            QVERIFY(mime->hasFormat(QString::fromLatin1(ChannelsWindow::kMimeType)));
            const auto drop = ChannelsWindow::parseMime(*mime);
            QVERIFY(drop.has_value());
            QCOMPARE(drop->channels, QStringList{QStringLiteral("Wheel Speed FL")});
            QCOMPARE(drop->newPanel, false);
            QMimeData foreign;
            foreign.setText(QStringLiteral("Throttle"));
            QVERIFY(!ChannelsWindow::parseMime(foreign).has_value());
            QMimeData panel;
            panel.setData(QString::fromLatin1(ChannelsWindow::kMimeType),
                          qt::ChannelsModel::channelsMime({QStringLiteral("A"), QStringLiteral("B")}, true));
            QCOMPARE(ChannelsWindow::parseMime(panel)->channels.size(), 2);
            QVERIFY(ChannelsWindow::parseMime(panel)->newPanel);
        }
        rt.shutdown();
    }

    void messagesFiltersAndActions() {
        qt::MessagesModel model;
        qt::CommandRegistry commands;
        int ran = 0;
        qt::CommandSpec spec;
        spec.id = QStringLiteral("view.channel_status");
        spec.title = QStringLiteral("Show Channel Status");
        spec.invoke = [&ran](const QVariantList&) { ++ran; };
        commands.add(spec);
        MessagesWindow win(model, commands);
        model.append(message(qt::Message::Severity::Info, qt::Message::Origin::Import, QStringLiteral("a.ld"),
                             QStringLiteral("alpha companion found")));
        model.append(message(qt::Message::Severity::Warning, qt::Message::Origin::Budget,
                             QStringLiteral("chunk cache"), QStringLiteral("beta over budget")));
        qt::Message withAction = message(qt::Message::Severity::Error, qt::Message::Origin::Maths,
                                         QStringLiteral("a.ld"), QStringLiteral("gamma failed"));
        withAction.code = QStringLiteral("E-CSD-NO-SPEED");
        withAction.action = QStringLiteral("view.channel_status");
        model.append(withAction);
        QCOMPARE(win.visibleCount(), 3);
        const auto* errors = win.findChild<QToolButton*>(QStringLiteral("messagesShowError"));
        QVERIFY(errors != nullptr);
        QCOMPARE(errors->text(), QStringLiteral("Errors (1)"));
        // Rows: time, origin, source, text and the action title.
        const QString row2 = win.filterModel().index(2, 0).data().toString();
        QVERIFY(row2.contains(QStringLiteral("Maths  a.ld: gamma failed")));
        QVERIFY(row2.contains(QStringLiteral("[Show Channel Status]")));
        QVERIFY(win.filterModel()
                    .index(2, 0)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("E-CSD-NO-SPEED")));

        win.setSeverityShown(MessagesWindow::Severity::Info, false);
        QCOMPARE(win.visibleCount(), 2);
        QVERIFY(!win.severityShown(MessagesWindow::Severity::Info));
        win.setSeverityShown(MessagesWindow::Severity::Info, true);
        win.filterEdit().setText(QStringLiteral("BETA"));
        QCOMPARE(win.visibleCount(), 1);
        win.filterEdit().setText(QStringLiteral("chunk")); // the source is searched too
        QCOMPARE(win.visibleCount(), 1);
        win.filterEdit().clear();

        const QString log = win.logText();
        QCOMPARE(log.count(QLatin1Char('\n')), 3);
        QVERIFY(log.contains(QStringLiteral("\tWarning\tBudget\tchunk cache\t\tbeta over budget\n")));
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("messages.log"));
        QVERIFY(win.saveAs(file));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(f.readAll()), log);

        // The action row: double-click and ENTER run its command; rows without one do nothing.
        QVERIFY(!win.invokeAction(win.filterModel().index(0, 0)));
        Q_EMIT win.list().doubleClicked(win.filterModel().index(2, 0));
        QCOMPARE(ran, 1);
        win.list().setCurrentIndex(win.filterModel().index(2, 0));
        QTest::keyClick(&win.list(), Qt::Key_Return);
        QCOMPARE(ran, 2);

        model.clear();
        QCOMPARE(win.visibleCount(), 0);
        QCOMPARE(errors->text(), QStringLiteral("Errors (0)"));
    }
};

QTEST_MAIN(TstChannelsMessages)
#include "tst_channels_messages.moc"
