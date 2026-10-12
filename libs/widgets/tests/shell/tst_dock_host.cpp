// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-13 on the QDockWidget backend (the only DockHost backend until ADS is vendored; 06 sections 3.1-3.5,
// D-026), offscreen: the 06 section 3.2 registry (Channels tabbed under Data, Messages auto-hidden), float
// and re-dock (by command and by the dock's own buttons), auto-hide with its side button and the fly-out of
// 06 section 3.3 item 5, toggle (V), save/restore of a perspective in the same window and in a fresh one,
// a refused foreign or damaged perspective, Reset Dock Windows, the dock focus signal, and the layout-1
// document (LayoutFile) round trip.

#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/layout_file.hpp>
#include <esses/widgets/shell/qdockwidget_backend.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <filesystem>
#include <memory>
#include <optional>

using namespace esses::widgets;

namespace {

const QString kData = QStringLiteral("data");
const QString kChannels = QStringLiteral("channels");
const QString kValues = QStringLiteral("values");
const QString kMessages = QStringLiteral("messages");

// A main window with the four M1 docks of 06 section 3.2 (contents: line edits, so focus can land in them).
struct Shell {
    QMainWindow host;
    QLineEdit* central = nullptr;
    DockHost* docks = nullptr;

    Shell() {
        host.resize(1000, 700);
        central = new QLineEdit;
        central->setObjectName(QStringLiteral("central"));
        docks = new DockHost(host, central);
        for (const auto& [id, title] :
             {std::pair{kData, QStringLiteral("Data")}, std::pair{kChannels, QStringLiteral("Channels")},
              std::pair{kValues, QStringLiteral("Values")},
              std::pair{kMessages, QStringLiteral("Messages")}}) {
            auto* content = new QLineEdit;
            content->setObjectName(id + QStringLiteral("Content"));
            const DockArea area = id == kValues     ? DockArea::Right
                                  : id == kMessages ? DockArea::Bottom
                                                    : DockArea::Left;
            const DockState state = id == kMessages ? DockState::AutoHide : DockState::Docked;
            docks->addDock(id, title, content, area, state);
        }
    }
    QDockWidget* dock(const QString& id) const {
        return qobject_cast<QDockWidget*>(docks->backend().container(id));
    }
    QDockWidgetBackend& backend() const { return static_cast<QDockWidgetBackend&>(docks->backend()); }
    bool show() {
        host.show();
        if (!QTest::qWaitForWindowExposed(&host)) {
            return false;
        }
        host.activateWindow();
        return QTest::qWaitForWindowActive(&host);
    }
};

} // namespace

class TstDockHost : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void stateNames() {
        for (const DockState s :
             {DockState::Docked, DockState::Floating, DockState::AutoHide, DockState::Closed}) {
            QCOMPARE(parseDockState(dockStateName(s)), std::optional<DockState>(s));
        }
        QCOMPARE(dockStateName(DockState::AutoHide), QStringLiteral("AutoHide"));
        QVERIFY(!parseDockState(QStringLiteral("Hidden")).has_value());
    }

    void registryDefaults() {
        Shell s;
        QVERIFY(s.show());
        QCOMPARE(QString::fromLatin1(s.docks->backend().name()), QStringLiteral("qdockwidget"));
        QCOMPARE(s.docks->dockIds(), (QStringList{kData, kChannels, kValues, kMessages}));
        QCOMPARE(s.docks->state(kData), DockState::Docked);
        QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
        QCOMPARE(s.docks->defaultArea(kValues), DockArea::Right);
        QCOMPARE(s.host.dockWidgetArea(s.dock(kData)), Qt::LeftDockWidgetArea);
        QCOMPARE(s.host.dockWidgetArea(s.dock(kValues)), Qt::RightDockWidgetArea);
        // Channels is tabbed under Data (06 section 3.2).
        QVERIFY(s.host.tabifiedDockWidgets(s.dock(kData)).contains(s.dock(kChannels)));
        // Messages: hidden, with a button on the bottom side bar.
        QVERIFY(s.dock(kMessages)->isHidden());
        QVERIFY(!s.docks->isVisible(kMessages));
        QAction* button = s.backend().autoHideButton(kMessages);
        QVERIFY(button != nullptr);
        QCOMPARE(button->text(), QStringLiteral("Messages"));
        QToolBar* bottom = s.backend().sideBar(DockArea::Bottom);
        QVERIFY(bottom != nullptr && bottom->isVisible());
        QCOMPARE(s.host.toolBarArea(bottom), Qt::BottomToolBarArea);
        QCOMPARE(s.docks->content(kValues)->objectName(), QStringLiteral("valuesContent"));
    }

    void floatAndDock() {
        Shell s;
        QVERIFY(s.show());
        QSignalSpy changed(s.docks, &DockHost::stateChanged);
        s.docks->setState(kValues, DockState::Floating);
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.dock(kValues)->isFloating());
        QVERIFY(s.dock(kValues)->isVisible());
        QVERIFY(changed.count() >= 1);
        s.docks->setState(kValues, DockState::Docked);
        QVERIFY(!s.dock(kValues)->isFloating());
        QCOMPARE(s.host.dockWidgetArea(s.dock(kValues)), Qt::RightDockWidgetArea);
        // The dock's own float button / a title-bar drag (no setState): the state follows.
        s.dock(kValues)->setFloating(true);
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        s.dock(kValues)->setFloating(false);
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
    }

    void autoHideFliesOutAndCollapses() {
        Shell s;
        QVERIFY(s.show());
        s.docks->setState(kData, DockState::AutoHide);
        QVERIFY(s.dock(kData)->isHidden());
        QAction* button = s.backend().autoHideButton(kData);
        QVERIFY(button != nullptr);
        QVERIFY(s.backend().sideBar(DockArea::Left)->isVisible());

        // Focused by command: flies out, state unchanged, focus inside (06 section 3.3 item 5).
        QSignalSpy focusSpy(s.docks, &DockHost::dockFocusChanged);
        s.docks->focus(kData);
        QVERIFY(s.dock(kData)->isVisible());
        QVERIFY(s.backend().isFlownOut(kData));
        QVERIFY(s.docks->isVisible(kData));
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QTRY_COMPARE(s.docks->focusedDockId(), kData);
        QVERIFY(button->isChecked());
        QVERIFY(!focusSpy.isEmpty());
        QCOMPARE(focusSpy.last().at(0).toString(), kData);
        QCOMPARE(focusSpy.last().at(1).toBool(), true);

        // Focus leaves the fly-out: it collapses and stays AutoHide.
        s.central->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(s.dock(kData)->isHidden());
        QVERIFY(!s.backend().isFlownOut(kData));
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QVERIFY(!button->isChecked());

        // The side button flies it out and in again.
        button->trigger();
        QVERIFY(s.dock(kData)->isVisible());
        QVERIFY(s.backend().isFlownOut(kData));
        button->trigger();
        QVERIFY(s.dock(kData)->isHidden());

        // Pinned again (Docked): the button and the empty side bar go.
        s.docks->setState(kData, DockState::Docked);
        QVERIFY(s.dock(kData)->isVisible());
        QVERIFY(s.backend().autoHideButton(kData) == nullptr);
        QVERIFY(!s.backend().sideBar(DockArea::Left)->isVisible());

        // show() restores an AutoHide dock to Docked.
        s.docks->show(kMessages);
        QCOMPARE(s.docks->state(kMessages), DockState::Docked);
        QVERIFY(s.dock(kMessages)->isVisible());
    }

    void toggleAndCloseButton() {
        Shell s;
        QVERIFY(s.show());
        QSignalSpy visibility(s.docks, &DockHost::visibilityChanged);
        s.docks->toggle(kValues); // V
        QCOMPARE(s.docks->state(kValues), DockState::Closed);
        QVERIFY(s.dock(kValues)->isHidden());
        QCOMPARE(visibility.count(), 1);
        QCOMPARE(visibility.last().at(1).toBool(), false);
        s.docks->toggle(kValues);
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
        QCOMPARE(visibility.last().at(1).toBool(), true);
        // Floating, then closed with the dock's own close button: toggle brings it back floating.
        s.docks->setState(kValues, DockState::Floating);
        QVERIFY(s.dock(kValues)->close());
        QCOMPARE(s.docks->state(kValues), DockState::Closed);
        QVERIFY(!s.docks->isVisible(kValues));
        s.docks->toggle(kValues);
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        // focus() on a Closed dock shows it.
        s.docks->setState(kChannels, DockState::Closed);
        s.docks->focus(kChannels);
        QCOMPARE(s.docks->state(kChannels), DockState::Docked);
        QTRY_COMPARE(s.docks->focusedDockId(), kChannels);
        QVERIFY(s.docks->focusedDock() == s.dock(kChannels));
    }

    void savesAndRestoresPerspective() {
        Shell s;
        QVERIFY(s.show());
        s.docks->setState(kValues, DockState::Floating);
        s.docks->setState(kData, DockState::AutoHide);
        s.docks->setState(kChannels, DockState::Closed);
        s.docks->setState(kMessages, DockState::Docked);
        // A user re-arrangement the perspective must keep: Messages moved to the right.
        s.host.addDockWidget(Qt::RightDockWidgetArea, s.dock(kMessages));
        const QByteArray perspective = s.docks->savePerspective();
        QVERIFY(!perspective.isEmpty());

        s.docks->resetToDefault();
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
        QCOMPARE(s.docks->state(kData), DockState::Docked);
        QCOMPARE(s.docks->state(kChannels), DockState::Docked);
        QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
        QVERIFY(!s.dock(kValues)->isFloating());
        QCOMPARE(s.host.dockWidgetArea(s.dock(kMessages)), Qt::BottomDockWidgetArea);

        QVERIFY(s.docks->restorePerspective(perspective));
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.dock(kValues)->isFloating());
        QVERIFY(s.dock(kValues)->isVisible());
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QVERIFY(s.dock(kData)->isHidden());
        QVERIFY(s.backend().autoHideButton(kData) != nullptr);
        QCOMPARE(s.docks->state(kChannels), DockState::Closed);
        QVERIFY(s.dock(kChannels)->isHidden());
        QCOMPARE(s.docks->state(kMessages), DockState::Docked);
        QVERIFY(s.dock(kMessages)->isVisible());
        QVERIFY(s.backend().autoHideButton(kMessages) == nullptr);
        QCOMPARE(s.host.dockWidgetArea(s.dock(kMessages)), Qt::RightDockWidgetArea);

        // The same perspective in a fresh window (session restore).
        Shell t;
        QVERIFY(t.show());
        QVERIFY(t.docks->restorePerspective(perspective));
        QCOMPARE(t.docks->state(kValues), DockState::Floating);
        QVERIFY(t.dock(kValues)->isFloating());
        QCOMPARE(t.docks->state(kData), DockState::AutoHide);
        QVERIFY(t.dock(kData)->isHidden());
        QCOMPARE(t.docks->state(kChannels), DockState::Closed);
        QCOMPARE(t.docks->state(kMessages), DockState::Docked);
        QCOMPARE(t.host.dockWidgetArea(t.dock(kMessages)), Qt::RightDockWidgetArea);
        // An auto-hidden dock restored on another side gets its button there.
        t.docks->focus(kData);
        QVERIFY(t.backend().isFlownOut(kData));
    }

    void refusesForeignPerspective() {
        Shell s;
        QVERIFY(s.show());
        s.docks->setState(kValues, DockState::Floating);
        QVERIFY(!s.docks->restorePerspective(QByteArray()));
        QVERIFY(!s.docks->restorePerspective(QByteArrayLiteral("not a perspective")));
        QVERIFY(!s.docks->restorePerspective(
            QByteArrayLiteral(R"({"backend":"ads","version":1,"mainWindow":"","docks":{}})")));
        QVERIFY(!s.docks->restorePerspective(
            QByteArrayLiteral(R"({"backend":"qdockwidget","version":1,"mainWindow":"AAAA","docks":{}})")));
        QCOMPARE(s.docks->state(kValues), DockState::Floating); // nothing changed
        QVERIFY(s.dock(kValues)->isFloating());
    }

    void layoutFileRoundTrip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path file =
            std::filesystem::path(dir.path().toStdWString()) / ".local" / "layout.json";
        LayoutFile missing;
        QVERIFY(LayoutFile::load(file, missing)); // a missing file is not an error
        QVERIFY(missing.perspective(QStringLiteral("_default")).isEmpty());

        LayoutFile out;
        out.setVersions(QStringLiteral("0.1.0"), QStringLiteral("6.7.2"), QStringLiteral("qdockwidget"));
        out.setWindow(QByteArrayLiteral("\x01\x02geometry"), true, QStringLiteral("DISPLAY1"), 1.5);
        out.setPerspective(QStringLiteral("_default"), QByteArrayLiteral("{\"p\":1}"));
        out.setPerspective(QStringLiteral("876b1936-3b4f-495d-b8f9-dfa0c067242f"),
                           QByteArrayLiteral("{\"p\":2}"));
        out.setActive(QStringLiteral("876b1936-3b4f-495d-b8f9-dfa0c067242f"), QStringLiteral("Driver"));
        out.setDockState(kMessages, QStringLiteral("Closed"));
        out.setDockOption(kValues, QStringLiteral("allChannels"), true);
        QString error;
        QVERIFY2(out.save(file, &error), qPrintable(error));

        LayoutFile in;
        QVERIFY(LayoutFile::load(file, in, &error));
        QCOMPARE(in.json().value(QStringLiteral("$schema")).toString(), QStringLiteral("urn:esses:layout-1"));
        QCOMPARE(in.json().value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(in.perspective(QStringLiteral("_default")), QByteArrayLiteral("{\"p\":1}"));
        QCOMPARE(in.perspective(QStringLiteral("876b1936-3b4f-495d-b8f9-dfa0c067242f")),
                 QByteArrayLiteral("{\"p\":2}"));
        QCOMPARE(in.windowGeometry(), QByteArrayLiteral("\x01\x02geometry"));
        QVERIFY(in.windowMaximised());
        QCOMPARE(in.activeWorkbook(),
                 std::optional<QString>(QStringLiteral("876b1936-3b4f-495d-b8f9-dfa0c067242f")));
        QCOMPARE(in.activeWorksheet(), std::optional<QString>(QStringLiteral("Driver")));
        QCOMPARE(in.dock(kMessages).value(QStringLiteral("state")).toString(), QStringLiteral("Closed"));
        QCOMPARE(in.dock(kValues).value(QStringLiteral("allChannels")).toBool(), true);
        QCOMPARE(in.appVersion(), QStringLiteral("0.1.0"));
        QCOMPARE(in.dockBackend(), QStringLiteral("qdockwidget"));
        QVERIFY(in.json()
                    .value(QStringLiteral("perspectives"))
                    .toObject()
                    .value(QStringLiteral("_default"))
                    .toObject()
                    .contains(QStringLiteral("savedAt")));
        in.removePerspective(QStringLiteral("_default"));
        QVERIFY(in.perspective(QStringLiteral("_default")).isEmpty());

        // Unknown members survive a save.
        {
            QFile f(QString::fromStdWString(file.wstring()));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(
                R"({"$schema":"urn:esses:layout-1","schemaVersion":1,"dialogs":{"lapEditor":{"geometry":"AA=="}}})");
        }
        LayoutFile kept;
        QVERIFY(LayoutFile::load(file, kept));
        kept.setPerspective(QStringLiteral("_default"), QByteArrayLiteral("x"));
        QVERIFY(kept.save(file));
        LayoutFile again;
        QVERIFY(LayoutFile::load(file, again));
        QVERIFY(
            again.json().value(QStringLiteral("dialogs")).toObject().contains(QStringLiteral("lapEditor")));

        // Damaged files and other schemas are refused with a message.
        for (const QByteArray& bad :
             {QByteArrayLiteral("{ not json"), QByteArrayLiteral("[1,2]"),
              QByteArrayLiteral(R"({"$schema":"urn:esses:settings-1","schemaVersion":1})")}) {
            QFile f(QString::fromStdWString(file.wstring()));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(bad);
            f.close();
            LayoutFile broken;
            QString message;
            QVERIFY(!LayoutFile::load(file, broken, &message));
            QVERIFY(!message.isEmpty());
            QVERIFY(broken.perspective(QStringLiteral("_default")).isEmpty());
        }
    }
};

QTEST_MAIN(TstDockHost)
#include "tst_dock_host.moc"
