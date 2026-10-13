// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M1-13 (06 sections 3.1-3.5, D-026, D-153), offscreen, on every DockHost backend of the build: the Qt
// Advanced Docking System backend ("ads", the default when the build has ADS) and the QDockWidget fallback
// ("qdockwidget"). Every test function runs once per backend (Qt Test global data, rows "ads" and
// "qdockwidget"): the 06 section 3.2 registry (Channels tabbed under Data, Messages auto-hidden), float and
// re-dock (by command and with the dock's own controls), auto-hide with its side button and the fly-out of
// 06 section 3.3 item 5, toggle (V), save/restore of a perspective in the same window and in a fresh one,
// refused foreign or damaged perspectives (and each backend refusing the other's), Reset Dock Windows, the
// dock focus signal, the worksheet area, the backend choice (ESSES_DOCK_BACKEND) and the layout-1 document
// (LayoutFile) round trip.
//
// What is checked is what a user sees, read from the widgets (is the dock's content on screen, in a window of
// its own, on which side of the worksheet, tabbed with which dock, is there a side-bar button), plus the
// DockHost state. A Probe per backend reads the backend's own widgets and drives its own controls
// (QDockWidget: setFloating, close, QMainWindow::addDockWidget; ADS: the dock area's undock and close
// buttons, the floating window's close, CDockManager::addDockWidget as a drop does, the side tab). Offscreen
// there is no window manager, so a floating dock is checked as a visible top-level window of its own, never
// by its position.

#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/layout_file.hpp>
#include <esses/widgets/shell/qdockwidget_backend.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <filesystem>
#include <memory>
#include <optional>

#if ESSES_HAVE_ADS
#include <esses/widgets/shell/ads_backend.hpp>

#include <AutoHideDockContainer.h>
#include <AutoHideSideBar.h>
#include <AutoHideTab.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>
#include <FloatingDockContainer.h>
#include <ads_globals.h>
#endif

using namespace esses::widgets;

namespace {

const QString kData = QStringLiteral("data");
const QString kChannels = QStringLiteral("channels");
const QString kValues = QStringLiteral("values");
const QString kMessages = QStringLiteral("messages");

// Lets queued layout requests and deferred deletes settle (splitter geometry, emptied floating windows).
void settle() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

// The widgets under `window` (windows themselves excluded) that have a native window of their own.
QStringList nativeChildren(const QWidget& window) {
    QStringList found;
    for (const QWidget* w : window.findChildren<QWidget*>()) {
        if (!w->isWindow() && w->internalWinId() != 0) {
            found.append(QString::fromLatin1(w->metaObject()->className()) + QLatin1Char(' ') +
                         w->objectName());
        }
    }
    return found;
}

// A main window with the four M1 docks of 06 section 3.2 (contents: line edits, so focus can land in them) on
// the named backend.
struct Shell {
    QMainWindow host;
    QLineEdit* central = nullptr;
    DockHost* docks = nullptr;

    // `withMessages` false: the Messages dock is not registered (a perspective saved before it existed).
    explicit Shell(const QString& backend, bool withMessages = true) {
        host.resize(1000, 700);
        central = new QLineEdit;
        central->setObjectName(QStringLiteral("central"));
        docks = new DockHost(host, central, DockHost::makeBackend(backend, host));
        for (const auto& [id, title] :
             {std::pair{kData, QStringLiteral("Data")}, std::pair{kChannels, QStringLiteral("Channels")},
              std::pair{kValues, QStringLiteral("Values")},
              std::pair{kMessages, QStringLiteral("Messages")}}) {
            if (id == kMessages && !withMessages) {
                continue;
            }
            auto* content = new QLineEdit;
            content->setObjectName(id + QStringLiteral("Content"));
            const DockArea area = id == kValues     ? DockArea::Right
                                  : id == kMessages ? DockArea::Bottom
                                                    : DockArea::Left;
            const DockState state = id == kMessages ? DockState::AutoHide : DockState::Docked;
            docks->addDock(id, title, content, area, state);
        }
    }
    bool show() {
        host.show();
        if (!QTest::qWaitForWindowExposed(&host)) {
            return false;
        }
        host.activateWindow();
        const bool active = QTest::qWaitForWindowActive(&host);
        settle();
        return active;
    }
    // The dock's content is on screen (docked and the current tab, floating, or a flown-out auto-hide dock).
    // QMainWindow keeps the other tabs of a tabbed group visible but moved out of sight, so the visible
    // region counts, after the pending layout.
    bool shown(const QString& id) const {
        settle();
        QWidget* c = docks->content(id);
        return c != nullptr && c->isVisible() && !c->visibleRegion().isEmpty();
    }
    // The dock is in a top-level window of its own.
    bool floatingWindow(const QString& id) const {
        QWidget* c = docks->content(id);
        return c != nullptr && c->window() != &host && c->window()->isVisible();
    }
};

// Backend-specific views and controls (see the file comment).
class Probe {
public:
    virtual ~Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    // The side of the worksheet area a docked dock is on.
    virtual std::optional<DockArea> side(const QString& id) const = 0;
    virtual bool tabbed(const QString& a, const QString& b) const = 0;
    // An open dock in a tabbed group has its tab (none is left hidden, so the user can select it).
    virtual bool tabShown(const QString& id) const = 0;
    // The side-bar button of an auto-hidden dock: whether it exists, its text, its side, whether it shows the
    // dock as flown out, a click on it, and whether the side bar of `side` is shown at all.
    virtual bool hasAutoHideButton(const QString& id) const = 0;
    virtual QString autoHideButtonText(const QString& id) const = 0;
    virtual std::optional<DockArea> autoHideButtonSide(const QString& id) const = 0;
    virtual bool autoHideButtonActive(const QString& id) const = 0;
    virtual void clickAutoHideButton(const QString& id) = 0;
    virtual bool sideBarShown(DockArea side) const = 0;
    // The dock's own controls: float it, dock it again (a drop on its area), close it, move it to another
    // side.
    virtual void userFloat(const QString& id) = 0;
    virtual void userRedock(const QString& id, DockArea area) = 0;
    virtual void userClose(const QString& id) = 0;
    virtual void userMove(const QString& id, DockArea area) = 0;
    // Perspectives this backend must refuse: another backend's, and damaged ones of its own.
    virtual QList<QByteArray> refusedPerspectives() const = 0;

protected:
    Probe() = default;
};

DockArea fromQtArea(Qt::DockWidgetArea a) {
    return a == Qt::RightDockWidgetArea    ? DockArea::Right
           : a == Qt::TopDockWidgetArea    ? DockArea::Top
           : a == Qt::BottomDockWidgetArea ? DockArea::Bottom
                                           : DockArea::Left;
}

Qt::DockWidgetArea toQtArea(DockArea a) {
    return a == DockArea::Right    ? Qt::RightDockWidgetArea
           : a == DockArea::Top    ? Qt::TopDockWidgetArea
           : a == DockArea::Bottom ? Qt::BottomDockWidgetArea
                                   : Qt::LeftDockWidgetArea;
}

class QDockWidgetProbe final : public Probe {
public:
    explicit QDockWidgetProbe(Shell& s) : s_(s) {}

    std::optional<DockArea> side(const QString& id) const override {
        QDockWidget* d = dock(id);
        if (d == nullptr || d->isFloating()) {
            return std::nullopt;
        }
        return fromQtArea(s_.host.dockWidgetArea(d));
    }
    bool tabbed(const QString& a, const QString& b) const override {
        return s_.host.tabifiedDockWidgets(dock(a)).contains(dock(b));
    }
    bool tabShown(const QString& id) const override { return dock(id) != nullptr && !dock(id)->isHidden(); }
    bool hasAutoHideButton(const QString& id) const override {
        return backend().autoHideButton(id) != nullptr;
    }
    QString autoHideButtonText(const QString& id) const override {
        QAction* a = backend().autoHideButton(id);
        return a == nullptr ? QString() : a->text();
    }
    std::optional<DockArea> autoHideButtonSide(const QString& id) const override {
        QAction* a = backend().autoHideButton(id);
        for (const DockArea side : {DockArea::Left, DockArea::Right, DockArea::Top, DockArea::Bottom}) {
            QToolBar* bar = backend().sideBar(side);
            if (a != nullptr && bar != nullptr && bar->actions().contains(a)) {
                return side;
            }
        }
        return std::nullopt;
    }
    bool autoHideButtonActive(const QString& id) const override {
        QAction* a = backend().autoHideButton(id);
        return a != nullptr && a->isChecked();
    }
    void clickAutoHideButton(const QString& id) override { backend().autoHideButton(id)->trigger(); }
    bool sideBarShown(DockArea side) const override {
        QToolBar* bar = backend().sideBar(side);
        return bar != nullptr && bar->isVisible();
    }
    void userFloat(const QString& id) override { dock(id)->setFloating(true); }
    void userRedock(const QString& id, DockArea area) override {
        dock(id)->setFloating(false);
        s_.host.addDockWidget(toQtArea(area), dock(id));
    }
    void userClose(const QString& id) override { QVERIFY(dock(id)->close()); }
    void userMove(const QString& id, DockArea area) override {
        s_.host.addDockWidget(toQtArea(area), dock(id));
    }
    QList<QByteArray> refusedPerspectives() const override {
        return {QByteArrayLiteral(R"({"backend":"ads","version":1,"adsVersion":"5.1.1","state":"AAAA"})"),
                QByteArrayLiteral(R"({"backend":"qdockwidget","version":1,"mainWindow":"AAAA","docks":{}})"),
                QByteArrayLiteral(R"({"backend":"qdockwidget","version":2,"mainWindow":"","docks":{}})")};
    }

private:
    QDockWidget* dock(const QString& id) const {
        return qobject_cast<QDockWidget*>(s_.docks->backend().container(id));
    }
    QDockWidgetBackend& backend() const { return static_cast<QDockWidgetBackend&>(s_.docks->backend()); }
    Shell& s_;
};

#if ESSES_HAVE_ADS
DockArea fromSideBar(ads::SideBarLocation s) {
    return s == ads::SideBarRight    ? DockArea::Right
           : s == ads::SideBarTop    ? DockArea::Top
           : s == ads::SideBarBottom ? DockArea::Bottom
                                     : DockArea::Left;
}

ads::SideBarLocation toSideBar(DockArea a) {
    return a == DockArea::Right    ? ads::SideBarRight
           : a == DockArea::Top    ? ads::SideBarTop
           : a == DockArea::Bottom ? ads::SideBarBottom
                                   : ads::SideBarLeft;
}

ads::DockWidgetArea toAdsArea(DockArea a) {
    return a == DockArea::Right    ? ads::RightDockWidgetArea
           : a == DockArea::Top    ? ads::TopDockWidgetArea
           : a == DockArea::Bottom ? ads::BottomDockWidgetArea
                                   : ads::LeftDockWidgetArea;
}

QRect rectIn(const QWidget* w, const QWidget* ancestor) {
    return {w->mapTo(ancestor, QPoint(0, 0)), w->size()};
}

class AdsProbe final : public Probe {
public:
    explicit AdsProbe(Shell& s) : s_(s) {}

    // Where the dock's area sits relative to the worksheet area, from the laid-out geometry.
    std::optional<DockArea> side(const QString& id) const override {
        settle();
        ads::CDockWidget* d = dock(id);
        if (d == nullptr || d->isClosed() || d->isAutoHide() || d->isInFloatingContainer() ||
            d->dockAreaWidget() == nullptr) {
            return std::nullopt;
        }
        const QRect r = rectIn(d->dockAreaWidget(), &s_.host);
        const QRect c = rectIn(s_.central, &s_.host);
        if (r.right() < c.left()) {
            return DockArea::Left;
        }
        if (r.left() > c.right()) {
            return DockArea::Right;
        }
        if (r.bottom() < c.top()) {
            return DockArea::Top;
        }
        if (r.top() > c.bottom()) {
            return DockArea::Bottom;
        }
        return std::nullopt; // overlapping the worksheet area: never
    }
    bool tabbed(const QString& a, const QString& b) const override {
        ads::CDockWidget* da = dock(a);
        ads::CDockWidget* db = dock(b);
        return da != nullptr && db != nullptr && da->dockAreaWidget() != nullptr &&
               da->dockAreaWidget() == db->dockAreaWidget();
    }
    bool tabShown(const QString& id) const override {
        ads::CDockWidget* d = dock(id);
        return d != nullptr && !d->isClosed() && d->tabWidget() != nullptr && !d->tabWidget()->isHidden();
    }
    bool hasAutoHideButton(const QString& id) const override { return tab(id) != nullptr; }
    QString autoHideButtonText(const QString& id) const override {
        ads::CAutoHideTab* t = tab(id);
        return t == nullptr ? QString() : t->text();
    }
    std::optional<DockArea> autoHideButtonSide(const QString& id) const override {
        ads::CAutoHideTab* t = tab(id);
        if (t == nullptr) {
            return std::nullopt;
        }
        return fromSideBar(t->sideBarLocation());
    }
    bool autoHideButtonActive(const QString& id) const override {
        ads::CAutoHideTab* t = tab(id);
        return t != nullptr && t->isActiveTab();
    }
    void clickAutoHideButton(const QString& id) override { tab(id)->click(); }
    bool sideBarShown(DockArea side) const override {
        ads::CAutoHideSideBar* bar = manager()->autoHideSideBar(toSideBar(side));
        return bar != nullptr && bar->isVisible();
    }
    // The undock button of the dock's area (the area holds only this dock in these tests).
    void userFloat(const QString& id) override {
        ads::CDockAreaWidget* area = dock(id)->dockAreaWidget();
        QVERIFY(area != nullptr);
        QCOMPARE(area->dockWidgetsCount(), 1);
        QAbstractButton* undock = area->titleBarButton(ads::TitleBarButtonUndock);
        QVERIFY(undock != nullptr);
        undock->click();
    }
    // A drop of the floating window on the main window's side does what CDockManager::addDockWidget does.
    void userRedock(const QString& id, DockArea area) override {
        manager()->addDockWidget(toAdsArea(area), dock(id));
    }
    // Docked: the close button of the dock's area; floating: the floating window's close button.
    void userClose(const QString& id) override {
        ads::CDockWidget* d = dock(id);
        if (ads::CFloatingDockContainer* window = d->floatingDockContainer()) {
            window->close();
            return;
        }
        QAbstractButton* close = d->dockAreaWidget()->titleBarButton(ads::TitleBarButtonClose);
        QVERIFY(close != nullptr);
        close->click();
    }
    void userMove(const QString& id, DockArea area) override {
        manager()->addDockWidget(toAdsArea(area), dock(id));
    }
    QList<QByteArray> refusedPerspectives() const override {
        const QByteArray wrongUserVersion = QByteArrayLiteral(
            R"(<?xml version="1.0"?><QtAdvancedDockingSystem Version="1" UserVersion="7" Containers="1"/>)");
        const QByteArray otherRoot =
            QByteArrayLiteral(R"(<?xml version="1.0"?><SomethingElse Version="1"/>)");
        return {QByteArrayLiteral(R"({"backend":"qdockwidget","version":1,"mainWindow":"AAAA","docks":{}})"),
                QByteArrayLiteral(R"({"backend":"ads","version":2,"adsVersion":"5.1.1","state":"AAAA"})"),
                QByteArrayLiteral(R"({"backend":"ads","version":1,"adsVersion":"5.1.1","state":""})"),
                QByteArrayLiteral(R"({"backend":"ads","version":1,"adsVersion":"5.1.1","state":"AAAAAAAA"})"),
                QByteArrayLiteral(R"({"backend":"ads","version":1,"state":")") + wrongUserVersion.toBase64() +
                    QByteArrayLiteral("\"}"),
                QByteArrayLiteral(R"({"backend":"ads","version":1,"state":")") + otherRoot.toBase64() +
                    QByteArrayLiteral("\"}")};
    }

private:
    ads::CDockWidget* dock(const QString& id) const {
        return qobject_cast<ads::CDockWidget*>(s_.docks->backend().container(id));
    }
    ads::CDockManager* manager() const {
        return qobject_cast<ads::CDockManager*>(static_cast<AdsBackend&>(s_.docks->backend()).dockManager());
    }
    ads::CAutoHideTab* tab(const QString& id) const {
        ads::CDockWidget* d = dock(id);
        return d == nullptr || !d->isAutoHide() ? nullptr : d->sideTabWidget();
    }
    Shell& s_;
};
#endif

std::unique_ptr<Probe> probeFor(Shell& s) {
#if ESSES_HAVE_ADS
    if (QLatin1String(s.docks->backend().name()) == QLatin1String("ads")) {
        return std::make_unique<AdsProbe>(s);
    }
#endif
    return std::make_unique<QDockWidgetProbe>(s);
}

} // namespace

class TstDockHost : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // Every test runs once per backend of the build.
    void initTestCase_data() {
        QTest::addColumn<QString>("backend");
        for (const QString& name : DockHost::availableBackends()) {
            QTest::newRow(qPrintable(name)) << name;
        }
    }

    void stateNames() {
        for (const DockState s :
             {DockState::Docked, DockState::Floating, DockState::AutoHide, DockState::Closed}) {
            QCOMPARE(parseDockState(dockStateName(s)), std::optional<DockState>(s));
        }
        QCOMPARE(dockStateName(DockState::AutoHide), QStringLiteral("AutoHide"));
        QVERIFY(!parseDockState(QStringLiteral("Hidden")).has_value());
    }

    // ESSES_DOCK_BACKEND: ADS is the build's default when it is built; the environment selects another
    // backend of the build; an unknown name is ignored with a warning.
    void backendChoice() {
        QFETCH_GLOBAL(QString, backend);
        const QStringList names = DockHost::availableBackends();
        QVERIFY(names.contains(backend));
        QCOMPARE(names.last(), QStringLiteral("qdockwidget"));
        QCOMPARE(names.contains(QStringLiteral("ads")), ESSES_HAVE_ADS != 0);
        const QString buildDefault = names.first();
        const QByteArray saved = qgetenv("ESSES_DOCK_BACKEND");
        qunsetenv("ESSES_DOCK_BACKEND");
        QCOMPARE(DockHost::defaultBackendName(), buildDefault);
        qputenv("ESSES_DOCK_BACKEND", backend.toUpper().toLatin1());
        QCOMPARE(DockHost::defaultBackendName(), backend);
        {
            QMainWindow host;
            const std::unique_ptr<IDockBackend> made = DockHost::makeDefaultBackend(host);
            QCOMPARE(QString::fromLatin1(made->name()), backend);
        }
        qputenv("ESSES_DOCK_BACKEND", "bogus");
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ESSES_DOCK_BACKEND=bogus")));
        QCOMPARE(DockHost::defaultBackendName(), buildDefault);
        if (saved.isNull()) {
            qunsetenv("ESSES_DOCK_BACKEND");
        } else {
            qputenv("ESSES_DOCK_BACKEND", saved);
        }
        QMainWindow host;
        QVERIFY(DockHost::makeBackend(QStringLiteral("bogus"), host) == nullptr);
        const std::unique_ptr<IDockBackend> made = DockHost::makeBackend(backend, host);
        QVERIFY(made != nullptr);
        QCOMPARE(QString::fromLatin1(made->name()), backend);
        QCOMPARE(made->libraryVersion().isEmpty(), backend != QStringLiteral("ads"));
    }

    void registryDefaults() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        QCOMPARE(QString::fromLatin1(s.docks->backend().name()), backend);
        QCOMPARE(s.docks->dockIds(), (QStringList{kData, kChannels, kValues, kMessages}));
        QCOMPARE(s.docks->state(kData), DockState::Docked);
        QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
        QCOMPARE(s.docks->defaultArea(kValues), DockArea::Right);
        QCOMPARE(p->side(kData), std::optional<DockArea>(DockArea::Left));
        QCOMPARE(p->side(kValues), std::optional<DockArea>(DockArea::Right));
        // Channels is tabbed under Data, and Data stays the current tab (06 section 3.2).
        QVERIFY(p->tabbed(kData, kChannels));
        QVERIFY(p->tabShown(kData));
        QVERIFY(p->tabShown(kChannels));
        QVERIFY(s.shown(kData));
        QVERIFY(!s.shown(kChannels));
        QVERIFY(s.shown(kValues));
        // Messages: not shown, with a button on the bottom side bar.
        QVERIFY(!s.shown(kMessages));
        QVERIFY(!s.docks->isVisible(kMessages));
        QVERIFY(p->hasAutoHideButton(kMessages));
        QCOMPARE(p->autoHideButtonText(kMessages), QStringLiteral("Messages"));
        QCOMPARE(p->autoHideButtonSide(kMessages), std::optional<DockArea>(DockArea::Bottom));
        QVERIFY(p->sideBarShown(DockArea::Bottom));
        QVERIFY(!p->autoHideButtonActive(kMessages));
        QCOMPARE(s.docks->content(kValues)->objectName(), QStringLiteral("valuesContent"));
        QCOMPARE(s.docks->dockIdOf(s.docks->content(kChannels)), kChannels);
    }

    // The worksheet area (the central widget) is never a dock: it stays on screen whatever the docks do.
    void worksheetArea() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        QVERIFY(s.central->isVisible());
        QCOMPARE(s.central->window(), &s.host);
        QVERIFY(s.docks->dockIdOf(s.central).isEmpty());
        s.central->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(s.central->hasFocus());
        QVERIFY(s.docks->focusedDockId().isEmpty());
        QVERIFY(s.docks->focusedDock() == nullptr);
        for (const QString& id : s.docks->dockIds()) {
            s.docks->setState(id, DockState::Floating);
        }
        QVERIFY(s.central->isVisible());
        for (const QString& id : s.docks->dockIds()) {
            s.docks->setState(id, DockState::Closed);
        }
        QVERIFY(s.central->isVisible());
        QCOMPARE(s.central->window(), &s.host);
#if ESSES_HAVE_ADS
        if (backend == QStringLiteral("ads")) {
            auto* manager =
                qobject_cast<ads::CDockManager*>(static_cast<AdsBackend&>(s.docks->backend()).dockManager());
            QVERIFY(manager != nullptr);
            QCOMPARE(s.host.centralWidget(), manager);
            ads::CDockWidget* area = manager->centralWidget();
            QVERIFY(area != nullptr);
            QCOMPARE(area->widget(), s.central);
            QVERIFY(!area->features().testFlag(ads::CDockWidget::DockWidgetClosable));
            QVERIFY(!area->features().testFlag(ads::CDockWidget::DockWidgetFloatable));
            QVERIFY(!area->features().testFlag(ads::CDockWidget::DockWidgetMovable));
            QVERIFY(!area->features().testFlag(ads::CDockWidget::DockWidgetPinnable));
        }
#endif
        if (backend == QStringLiteral("qdockwidget")) {
            QCOMPARE(s.host.centralWidget(), s.central);
        }
    }

    void floatAndDock() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        QSignalSpy changed(s.docks, &DockHost::stateChanged);
        s.docks->setState(kValues, DockState::Floating);
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.floatingWindow(kValues));
        QVERIFY(s.shown(kValues));
        QVERIFY(s.docks->isVisible(kValues));
        QVERIFY(changed.count() >= 1);
        s.docks->setState(kValues, DockState::Docked);
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
        settle();
        QVERIFY(!s.floatingWindow(kValues));
        QVERIFY(s.shown(kValues));
        QCOMPARE(p->side(kValues), std::optional<DockArea>(DockArea::Right));

        // The dock's own controls (no setState): the state follows and DockHost announces it.
        changed.clear();
        p->userFloat(kValues);
        QTRY_COMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.floatingWindow(kValues));
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(changed.last().at(0).toString(), kValues);
        changed.clear();
        p->userRedock(kValues, DockArea::Right);
        QTRY_COMPARE(s.docks->state(kValues), DockState::Docked);
        settle();
        QVERIFY(!s.floatingWindow(kValues));
        QCOMPARE(p->side(kValues), std::optional<DockArea>(DockArea::Right));
        QTRY_VERIFY(!changed.isEmpty());

        // Off X11 floating leaves no native window inside the main window. ADS's Linux floating window
        // used to make the dock manager's whole tree native, and the offscreen platform's backing store then
        // crashed when this main window was destroyed.
        if (QGuiApplication::platformName() != QLatin1String("xcb")) {
            QCOMPARE(nativeChildren(s.host), QStringList());
        }
    }

    void autoHideFliesOutAndCollapses() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        s.docks->setState(kData, DockState::AutoHide);
        settle();
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QVERIFY(!s.shown(kData));
        QVERIFY(p->hasAutoHideButton(kData));
        QCOMPARE(p->autoHideButtonSide(kData), std::optional<DockArea>(DockArea::Left));
        QVERIFY(p->sideBarShown(DockArea::Left));
        // Channels, tabbed with Data, stays docked.
        QCOMPARE(s.docks->state(kChannels), DockState::Docked);
        QVERIFY(s.shown(kChannels));

        // Focused by command: flies out, state unchanged, focus inside (06 section 3.3 item 5).
        QSignalSpy focusSpy(s.docks, &DockHost::dockFocusChanged);
        QSignalSpy visibility(s.docks, &DockHost::visibilityChanged);
        s.docks->focus(kData);
        QVERIFY(s.shown(kData));
        QVERIFY(s.docks->backend().isFlownOut(kData));
        QVERIFY(s.docks->isVisible(kData));
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QTRY_COMPARE(s.docks->focusedDockId(), kData);
        QVERIFY(p->autoHideButtonActive(kData));
        QVERIFY(!focusSpy.isEmpty());
        QCOMPARE(focusSpy.last().at(0).toString(), kData);
        QCOMPARE(focusSpy.last().at(1).toBool(), true);
        QVERIFY(!visibility.isEmpty());
        QCOMPARE(visibility.last().at(1).toBool(), true);

        // Focus leaves the fly-out: it collapses, stays AutoHide, and DockHost announces it.
        visibility.clear();
        s.central->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(!s.shown(kData));
        QVERIFY(!s.docks->backend().isFlownOut(kData));
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QVERIFY(!p->autoHideButtonActive(kData));
        QTRY_VERIFY(!visibility.isEmpty());
        QCOMPARE(visibility.last().at(0).toString(), kData);
        QCOMPARE(visibility.last().at(1).toBool(), false);

        // The side button flies it out and in again.
        p->clickAutoHideButton(kData);
        QTRY_VERIFY(s.shown(kData));
        QVERIFY(s.docks->backend().isFlownOut(kData));
        p->clickAutoHideButton(kData);
        QTRY_VERIFY(!s.shown(kData));
        QVERIFY(!s.docks->backend().isFlownOut(kData));

        // Pinned again (Docked): the button and the empty side bar go.
        s.docks->setState(kData, DockState::Docked);
        settle();
        QCOMPARE(s.docks->state(kData), DockState::Docked);
        QVERIFY(s.shown(kData));
        QVERIFY(!p->hasAutoHideButton(kData));
        QVERIFY(!p->sideBarShown(DockArea::Left));
        QCOMPARE(p->side(kData), std::optional<DockArea>(DockArea::Left));

        // show() restores an AutoHide dock to Docked.
        s.docks->show(kMessages);
        settle();
        QCOMPARE(s.docks->state(kMessages), DockState::Docked);
        QVERIFY(s.shown(kMessages));
        QVERIFY(!p->hasAutoHideButton(kMessages));
    }

    void toggleAndCloseButton() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        QSignalSpy visibility(s.docks, &DockHost::visibilityChanged);
        s.docks->toggle(kValues); // V
        QCOMPARE(s.docks->state(kValues), DockState::Closed);
        QVERIFY(!s.shown(kValues));
        QCOMPARE(visibility.count(), 1);
        QCOMPARE(visibility.last().at(1).toBool(), false);
        s.docks->toggle(kValues);
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
        QVERIFY(s.shown(kValues));
        QCOMPARE(visibility.last().at(1).toBool(), true);
        // Floating, then closed with the window's own close button: toggle brings it back floating.
        s.docks->setState(kValues, DockState::Floating);
        p->userClose(kValues);
        QTRY_COMPARE(s.docks->state(kValues), DockState::Closed);
        QVERIFY(!s.docks->isVisible(kValues));
        QVERIFY(!s.shown(kValues));
        s.docks->toggle(kValues);
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.floatingWindow(kValues));
        // A docked dock closed with its own close button.
        s.docks->setState(kValues, DockState::Docked);
        p->userClose(kValues);
        QTRY_COMPARE(s.docks->state(kValues), DockState::Closed);
        QTRY_COMPARE(visibility.last().at(1).toBool(), false);
        // focus() on a Closed dock shows it.
        s.docks->setState(kChannels, DockState::Closed);
        QVERIFY(!s.shown(kChannels));
        s.docks->focus(kChannels);
        QCOMPARE(s.docks->state(kChannels), DockState::Docked);
        QVERIFY(s.shown(kChannels));
        QVERIFY(p->tabbed(kData, kChannels));
        QVERIFY(p->tabShown(kChannels));
        QVERIFY(p->tabShown(kData));
        QTRY_COMPARE(s.docks->focusedDockId(), kChannels);
        QVERIFY(s.docks->focusedDock() == s.docks->backend().container(kChannels));
    }

    void savesAndRestoresPerspective() {
        QFETCH_GLOBAL(QString, backend);
        QByteArray perspective;
        {
            Shell s(backend);
            QVERIFY(s.show());
            const std::unique_ptr<Probe> p = probeFor(s);
            s.docks->setState(kValues, DockState::Floating);
            s.docks->setState(kData, DockState::AutoHide);
            s.docks->setState(kChannels, DockState::Closed);
            s.docks->setState(kMessages, DockState::Docked);
            // A user re-arrangement the perspective must keep: Messages moved to the right.
            p->userMove(kMessages, DockArea::Right);
            QCOMPARE(p->side(kMessages), std::optional<DockArea>(DockArea::Right));
            perspective = s.docks->savePerspective();
            QVERIFY(!perspective.isEmpty());

            s.docks->resetToDefault();
            settle();
            QCOMPARE(s.docks->state(kValues), DockState::Docked);
            QCOMPARE(s.docks->state(kData), DockState::Docked);
            QCOMPARE(s.docks->state(kChannels), DockState::Docked);
            QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
            QVERIFY(!s.floatingWindow(kValues));
            QCOMPARE(p->side(kValues), std::optional<DockArea>(DockArea::Right));
            QCOMPARE(p->side(kData), std::optional<DockArea>(DockArea::Left));
            QVERIFY(p->tabbed(kData, kChannels));
            QVERIFY(p->tabShown(kData));
            QVERIFY(p->tabShown(kChannels)); // it was closed before the reset
            QCOMPARE(p->autoHideButtonSide(kMessages), std::optional<DockArea>(DockArea::Bottom));

            QVERIFY(s.docks->restorePerspective(perspective));
            settle();
            QCOMPARE(s.docks->state(kValues), DockState::Floating);
            QVERIFY(s.floatingWindow(kValues));
            QVERIFY(s.shown(kValues));
            QCOMPARE(s.docks->state(kData), DockState::AutoHide);
            QVERIFY(!s.shown(kData));
            QVERIFY(p->hasAutoHideButton(kData));
            QCOMPARE(s.docks->state(kChannels), DockState::Closed);
            QVERIFY(!s.shown(kChannels));
            QCOMPARE(s.docks->state(kMessages), DockState::Docked);
            QVERIFY(s.shown(kMessages));
            QVERIFY(!p->hasAutoHideButton(kMessages));
            QCOMPARE(p->side(kMessages), std::optional<DockArea>(DockArea::Right));
        }

        // The same perspective in a fresh window (session restore).
        Shell t(backend);
        QVERIFY(t.show());
        const std::unique_ptr<Probe> q = probeFor(t);
        QVERIFY(t.docks->restorePerspective(perspective));
        settle();
        QCOMPARE(t.docks->state(kValues), DockState::Floating);
        QVERIFY(t.floatingWindow(kValues));
        QCOMPARE(t.docks->state(kData), DockState::AutoHide);
        QVERIFY(!t.shown(kData));
        QCOMPARE(q->autoHideButtonSide(kData), std::optional<DockArea>(DockArea::Left));
        QCOMPARE(t.docks->state(kChannels), DockState::Closed);
        QCOMPARE(t.docks->state(kMessages), DockState::Docked);
        QCOMPARE(q->side(kMessages), std::optional<DockArea>(DockArea::Right));
        // The restored auto-hidden dock flies out by command; a closed one opens where it was saved.
        t.docks->focus(kData);
        QVERIFY(t.docks->backend().isFlownOut(kData));
        QVERIFY(t.shown(kData));
        t.docks->focus(kChannels); // the main window becomes active again (Values floats and was active)
        QCOMPARE(t.docks->state(kChannels), DockState::Docked);
        QVERIFY(t.shown(kChannels));
        QVERIFY(q->tabShown(kChannels));
        QTRY_COMPARE(t.docks->focusedDockId(), kChannels);
        QTRY_VERIFY(!t.docks->backend().isFlownOut(kData));
    }

    void refusesForeignPerspective() {
        QFETCH_GLOBAL(QString, backend);
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        s.docks->setState(kValues, DockState::Floating);
        s.docks->setState(kData, DockState::AutoHide);
        QVERIFY(!s.docks->restorePerspective(QByteArray()));
        QVERIFY(!s.docks->restorePerspective(QByteArrayLiteral("not a perspective")));
        for (const QByteArray& bad : p->refusedPerspectives()) {
            QVERIFY2(!s.docks->restorePerspective(bad), bad.constData());
        }
        settle();
        // Nothing changed.
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.floatingWindow(kValues));
        QCOMPARE(s.docks->state(kData), DockState::AutoHide);
        QCOMPARE(s.docks->state(kChannels), DockState::Docked);
        QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
    }

    // A dock registered after the perspective was saved (a newer version's dock) keeps its registered area
    // and state; the others are restored.
    void perspectiveWithoutADock() {
        QFETCH_GLOBAL(QString, backend);
        QByteArray older;
        {
            Shell o(backend, false);
            QVERIFY(o.show());
            o.docks->setState(kValues, DockState::Floating);
            o.docks->setState(kChannels, DockState::Closed);
            older = o.docks->savePerspective();
        }
        Shell s(backend);
        QVERIFY(s.show());
        const std::unique_ptr<Probe> p = probeFor(s);
        QVERIFY(s.docks->restorePerspective(older));
        settle();
        QCOMPARE(s.docks->state(kValues), DockState::Floating);
        QVERIFY(s.floatingWindow(kValues));
        QCOMPARE(s.docks->state(kChannels), DockState::Closed);
        QCOMPARE(s.docks->state(kData), DockState::Docked);
        QCOMPARE(s.docks->state(kMessages), DockState::AutoHide);
        QVERIFY(!s.shown(kMessages));
        QCOMPARE(p->autoHideButtonSide(kMessages), std::optional<DockArea>(DockArea::Bottom));
        s.docks->focus(kMessages);
        QVERIFY(s.shown(kMessages));
        s.docks->setState(kMessages, DockState::Docked);
        QVERIFY(s.shown(kMessages));
        QCOMPARE(p->side(kMessages), std::optional<DockArea>(DockArea::Bottom));
    }

    // A perspective written by the other backend (a layout-1 file of another build) is refused, so the shell
    // falls back to the default layout (D-153).
    void otherBackendsPerspective() {
        QFETCH_GLOBAL(QString, backend);
        const QStringList names = DockHost::availableBackends();
        if (names.size() < 2) {
            QSKIP("this build has one dock backend");
        }
        const QString other = names.at(names.indexOf(backend) == 0 ? 1 : 0);
        QByteArray theirs;
        {
            Shell o(other);
            QVERIFY(o.show());
            o.docks->setState(kValues, DockState::Floating);
            theirs = o.docks->savePerspective();
        }
        Shell s(backend);
        QVERIFY(s.show());
        QVERIFY(!s.docks->restorePerspective(theirs));
        QCOMPARE(s.docks->state(kValues), DockState::Docked);
        QVERIFY(!s.floatingWindow(kValues));
        QVERIFY(s.docks->restorePerspective(s.docks->savePerspective()));
    }

    void layoutFileRoundTrip() {
        QFETCH_GLOBAL(QString, backend);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path file =
            std::filesystem::path(dir.path().toStdWString()) / ".local" / "layout.json";
        LayoutFile missing;
        QVERIFY(LayoutFile::load(file, missing)); // a missing file is not an error
        QVERIFY(missing.perspective(QStringLiteral("_default")).isEmpty());

        // The versions as MainWindow::saveLayout writes them for this backend.
        QString adsVersion;
        {
            QMainWindow host;
            adsVersion = DockHost::makeBackend(backend, host)->libraryVersion();
        }
        LayoutFile out;
        out.setVersions(QStringLiteral("0.1.0"), QStringLiteral("6.7.2"), backend, adsVersion);
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
        QCOMPARE(in.dockBackend(), backend);
        // 06 section 3.4: "adsVersion" only when ADS wrote the perspectives.
        QCOMPARE(in.adsVersion(), adsVersion);
        QCOMPARE(in.json().contains(QStringLiteral("adsVersion")), backend == QStringLiteral("ads"));
        if (backend == QStringLiteral("ads")) {
            QCOMPARE(in.adsVersion(), QStringLiteral("5.1.1"));
            in.setVersions(QStringLiteral("0.1.0"), QStringLiteral("6.7.2"), QStringLiteral("qdockwidget"));
            QVERIFY(!in.json().contains(QStringLiteral("adsVersion")));
        }
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
