// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DprWatcher (M4 hidpi package; D-206, D-209; 07 section 7.1 row 3 "per-monitor DPR change relayout",
// AC-M4-07; 06 section 17 items 3-4; 04 section 6.6; D-025): follows the device pixel ratio of top-level
// windows: the main window and every floating dock window (ADS floating containers and floating QDockWidgets
// are top-level windows with their own screen). When a watched window moves to a screen of another DPR, or
// its DPR changes in place (a display's scale changed), it re-checks what the window holds: every
// ComponentWidget under it is asked to re-check its DPR (ComponentWidget::recheckDevicePixelRatio: a full
// Size|Scheme rebuild at the new DPR, the key column laid out again) and every WorksheetCanvas re-applies its
// 1 % grid placement (WorksheetCanvas::refreshGeometry), so nothing is drawn from an image of the old DPR (no
// blur) and nothing keeps a layout of the old DPR (no drift; the placement is logical and stays the same).
// Qt's own QEvent::DevicePixelRatioChange / ScreenChangeInternal reach the widgets of the window as well; the
// watcher is the path that does not depend on Qt notifying a widget: a widget moved into another window (a
// dock floated onto, or docked back from, a monitor of another DPR) gets no DPR event from Qt
// (QWidget::setParent sends ParentChange only), and a floating window created on a screen whose DPR equals
// the QWindow's initial 1.0 gets none either.
// How it listens: an event filter on each watched top-level widget (QWidgetWindow forwards the window's
// DevicePixelRatioChange and ScreenChangeInternal to it first, and its Show arrives when a floating window
// appears), so no QWindow is forced into existence; each such event queues one coalesced check of that window
// for the next event-loop turn (after Qt has delivered its own recursive DPR events). A check re-checks the
// window's components and canvases; when the window's DPR differs from the last one seen it also counts a
// change (changes()) and emits dprChanged. A destroyed window is forgotten. GUI thread only.
// Files of the package (docs/milestones/M4.md section 3): src/shell/m4_hidpi/,
// src/shell/main_window_m4_hidpi.cpp, src/components/m4_hidpi/, this header and hidpi_shell.hpp.
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <vector>

class QEvent;
class QWidget;

namespace esses::widgets {

class DprWatcher final : public QObject {
    Q_OBJECT
public:
    explicit DprWatcher(QObject* parent = nullptr);
    ~DprWatcher() override;
    DprWatcher(const DprWatcher&) = delete;
    DprWatcher& operator=(const DprWatcher&) = delete;

    // Follows the window of `topLevel` (topLevel->window(); nullptr and an already watched window are
    // ignored). Its current DPR is the first one seen.
    void watch(QWidget* topLevel);
    // Stops following the window of `topLevel` (its event filter goes).
    void unwatch(QWidget* topLevel);
    // The watched windows, in watch order.
    QList<QWidget*> watched() const;
    // The DPR last seen for a watched window (0 when not watched).
    qreal lastDpr(const QWidget* topLevel) const;
    // DPR changes of watched windows seen so far (tests).
    int changes() const;
    // Checks every watched window now (HiDpiShell after a dock floated or docked: the moved widgets got no
    // DPR event from Qt); returns the number of components asked to rebuild at their window's DPR.
    int refreshAll();
    // Re-checks every ComponentWidget under `root` (root included) and refreshes the WorksheetCanvases there;
    // returns the number of components that were not at their window's DPR and were asked to rebuild
    // (ComponentWidget::recheckDevicePixelRatio returned true).
    static int refreshComponents(QWidget& root);

Q_SIGNALS:
    void dprChanged(QWidget* topLevel, qreal from, qreal to);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QPointer<QWidget> window;
        const QObject* key = nullptr; // the address, to forget a window while it is being destroyed
        qreal dpr = 0.0;
        bool queued = false;
    };
    Entry* find(const QObject* window);
    void queueCheck(QWidget* window);
    int check(QWidget* window);
    void forget(const QObject* window);

    std::vector<Entry> entries_;
    int changes_ = 0;
};

} // namespace esses::widgets
