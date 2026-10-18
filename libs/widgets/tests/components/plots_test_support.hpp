// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Header-only helpers of the Qt Tests of the plots components (tst_scatter_plot, tst_histogram,
// tst_outing_graph, tst_gauges): specs from JSON bodies, a component in a plain host widget whose spec edits
// are applied the way the canvas applies them (without the undo stack), waiting for a settled Static layer,
// mouse and wheel events, the text runs of a DrawList, an ExportSink into memory and the values of the
// synthetic sources of component_test_support (computed here from their formulas, not by the code under
// test).
#pragma once

#include "components/component_test_support.hpp"

#include <esses/base/uuid.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/layered_component.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace esses::widgets::plotstest {

inline constexpr Micros kSec = 1'000'000;
inline constexpr int kTimeoutMs = 30'000;

inline project::ComponentSpec specOf(const char* type, const char* bodyJson) {
    project::ComponentSpec spec;
    spec.id = uuidRandom();
    spec.type = type;
    spec.body = JsonDoc::parse(bodyJson).value();
    return spec;
}

// Main = `ref` with the zoom on it (W) and the cursor at its start.
inline void selectMain(cmptest::World& w, const model::RangeRef& ref) {
    w.selection.setMain(ref);
    w.view->zoomToMainRange();
    w.view->setCursor(0, qt::CursorSource::Programmatic);
}

// One component in a plain host widget; its spec edits are applied at once (the canvas's part without the
// undo stack), and the last undo text is kept.
template <class T> struct Rig {
    Rig(cmptest::World& world, const project::ComponentSpec& spec, QSize size = {420, 300}) {
        host = std::make_unique<QWidget>();
        host->resize(size);
        c = new T(spec, *world.context, host.get());
        c->setGeometry(QRect(QPoint(0, 0), size));
        QObject::connect(c, &ComponentWidget::specEditRequested, c,
                         [this](const project::ComponentSpec& next, const QString& undoText) {
                             ++edits;
                             lastUndoText = undoText;
                             c->applySpec(next);
                         });
        host->show();
    }
    std::unique_ptr<QWidget> host;
    T* c = nullptr;
    int edits = 0;
    QString lastUndoText;
};

// Waits until the component is settled (cmptest::settled: its pool idle with every result delivered, so no
// late maths/distance result can rebuild it any more, and no Static build pending) and stays so for `quietMs`
// (a GUI timer, such as the trailing edge of a resize burst, may still request a build). `C` is a
// LayeredComponent or a TimeDistanceGraph. A build replaced while queued is never delivered (01 section 5.4
// rule 2): compare staticBuildShown(), not staticBuildsDelivered(), with staticBuildsSubmitted().
template <class C> bool waitQuiet(C& g, int quietMs = 150) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kTimeoutMs) {
        if (!QTest::qWaitFor([&g] { return cmptest::settled(g); }, kTimeoutMs)) {
            return false;
        }
        const int submitted = g.staticBuildsSubmitted();
        QTest::qWait(quietMs);
        if (cmptest::settled(g) && g.staticBuildsSubmitted() == submitted) {
            return true;
        }
    }
    return false;
}

// After an action that must leave the Static layer alone, on a component settled beforehand (waitQuiet: the
// idle pool can no longer rebuild it): the Static layer is invalidated synchronously, so a rebuild shows at
// once as !staticUpToDate(); the request it would have queued is then delivered and must not have submitted a
// build, an exact count since nothing else arrives meanwhile (a fixed wait proves neither).
template <class C> bool staticUntouched(C& g, int submittedBefore) {
    if (!g.staticUpToDate()) {
        return false;
    }
    cmptest::drainEvents();
    return g.staticUpToDate() && g.staticBuildsSubmitted() == submittedBefore;
}

inline void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button,
                      Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

inline void click(QWidget* w, QPointF pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

inline void doubleClick(QWidget* w, QPointF pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
    sendMouse(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

inline void sendWheel(QWidget* w, QPointF pos, int deltaY) {
    QWheelEvent e(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, deltaY), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

inline void sendKey(QWidget* w, int key) {
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

inline std::vector<std::string> textsOf(const plot::DrawList& list, plot::TextStyleId style) {
    std::vector<std::string> out;
    for (const plot::TextRun& r : list.texts) {
        if (r.style == style) {
            out.emplace_back(list.textOf(r));
        }
    }
    return out;
}

inline int countCmds(const plot::DrawList& list, plot::CmdKind kind, project::Rgba colour = 0) {
    int n = 0;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind == kind && (colour == 0 || c.colour == colour)) {
            ++n;
        }
    }
    return n;
}

class MemorySink final : public ExportSink {
public:
    bool write(const QByteArray& b) override {
        bytes += b;
        return true;
    }
    QByteArray bytes;
};

inline QList<QByteArray> csvLines(const QByteArray& csv) {
    QList<QByteArray> lines = csv.split('\n');
    while (!lines.isEmpty() && lines.back().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

// The synthetic sources of component_test_support::makeSource (default options: laps of 40, 50, 48, 49 and
// 30 s, 50 Hz, a 2000 m lap): the throttle in % and the front-left wheel speed in m/s at sample i.
struct SourceShape {
    std::vector<double> lapSeconds{40.0, 50.0, 48.0, 49.0, 30.0};
    double trackLengthM = 2000.0;
    unsigned rateHz = 50;
    double throttlePhase = 0.0;

    // (lap index, lap start in s) of time t (s).
    std::pair<std::size_t, double> lapOf(double t) const {
        double start = 0.0;
        std::size_t lap = 0;
        while (lap + 1 < lapSeconds.size() && t >= start + lapSeconds[lap]) {
            start += lapSeconds[lap];
            ++lap;
        }
        return {lap, start};
    }
    double throttle(std::size_t i) const {
        const double t = static_cast<double>(i) / rateHz;
        const auto [lap, start] = lapOf(t);
        const double u = (t - start) / lapSeconds[lap];
        return static_cast<double>(
            static_cast<float>(50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * u + throttlePhase)));
    }
    double wheelFl(std::size_t i) const {
        const double t = static_cast<double>(i) / rateHz;
        const auto [lap, start] = lapOf(t);
        const double period = lapSeconds[lap];
        const double u = (t - start) / period;
        return static_cast<double>(
            static_cast<float>(trackLengthM / period * (1.0 + 0.3 * std::sin(2.0 * std::numbers::pi * u))));
    }
    double lapStart(std::size_t lap) const {
        double s = 0.0;
        for (std::size_t k = 0; k < lap; ++k) {
            s += lapSeconds[k];
        }
        return s;
    }
};

} // namespace esses::widgets::plotstest
