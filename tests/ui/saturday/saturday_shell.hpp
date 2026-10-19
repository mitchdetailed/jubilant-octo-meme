// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shell helpers of the Saturday walkthrough (ui.tst_walkthrough_saturday, AC-M4-01; D-214), header-only and
// included by tst_walkthrough_saturday.cpp alone: a Day (a runtime, the real MainWindow on a private
// configuration directory with the shipped data, a project folder, a copy of the day's logs), keys sent the
// way the platform does (portable text through the KeyEventRouter), the components of the shown worksheet,
// the numbers a DrawList shows, values of a series at a time (read on purpose under ThreadRole::Normal), the
// project's journal and backup files, and the WCAG contrast ratio of two colours (06 section 20 item 3,
// section 13.9 item 4).
#pragma once

#include "../ui_test_support.hpp"
#include "saturday_log.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/model/range.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/store/series.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QColor>
#include <QKeySequence>
#include <QString>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace esses::saturday {

inline constexpr int kTimeoutMs = 90'000;

inline std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

// The real shell over a copy of the day's logs: a runtime as esses has it (GUI thread NoBlock), MainWindow on
// a private configuration directory, optionally a new project "Saturday" opened in it.
struct Day {
    QTemporaryDir root;
    QTemporaryDir config;
    std::unique_ptr<qt::AppRuntime> rt;
    std::unique_ptr<widgets::MainWindow> w;
    std::filesystem::path folder; // the project folder ("" without a project)
    std::filesystem::path logs;
    std::filesystem::path ld; // the copy of the day's .ld (its .ldx beside it)
    model::SourceId src;
    std::vector<model::RangeRef> laps; // displayed order: out lap, 70 s, 68 s, 68.5 s, in lap

    Day() = default;
    Day(const Day&) = delete;
    Day& operator=(const Day&) = delete;
    ~Day() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }

    // Copies the day's logs from `master` (a directory with saturday.ld and saturday.ldx), starts the runtime
    // and shows the window; with `project`, a new project "Saturday" opened in it.
    bool start(const std::filesystem::path& master, bool project) {
        if (!root.isValid() || !config.isValid()) {
            return false;
        }
        logs = fsPath(root.path()) / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logs, ec);
        for (const char* f : {"saturday.ld", "saturday.ldx"}) {
            std::filesystem::copy_file(master / f, logs / f,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return false;
            }
        }
        ld = logs / "saturday.ld";
        rt = uitest::makeRuntime();
        w = std::make_unique<widgets::MainWindow>(*rt, uitest::pathsFor(config.path()));
        w->setExitPrompt([] { return widgets::MainWindow::ExitAnswer::Discard; });
        w->resize(1400, 900);
        if (project) {
            folder = fsPath(root.path()) / "Saturday";
            if (!w->session().newProject(folder, "Saturday").ok() || !w->openProject(folder)) {
                return false;
            }
        }
        return uitest::showAndActivate(*w);
    }

    // Opens the day's .ld (its .ldx merged) and waits for it to be published with its distance index.
    bool openDay() {
        const std::size_t before = w->selection().sources().size();
        w->openLog(ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == before + 1; }, kTimeoutMs)) {
            return false;
        }
        for (const auto& s : w->selection().sources()) {
            if (s->prov.path.filename() == ld.filename()) {
                src = s->id;
            }
        }
        if (!QTest::qWaitFor([&] { return w->distance().state(src) == qt::DistanceState::Ready; },
                             kTimeoutMs)) {
            return false;
        }
        laps = w->selection().displayedRanges(src);
        return laps.size() == kLapSeconds.size();
    }

    // Main = the 68 s lap, Overlay 0 = the 68.5 s lap.
    void selectMainAndReference() {
        w->selection().setMain(laps[kFastest]);
        if (!(w->selection().snapshot().overlay0 == std::optional<model::RangeRef>(laps[3]))) {
            w->selection().toggleOverlay0(laps[3]);
        }
    }

    // The absolute time of `secondsIntoMain` seconds into the Main lap.
    Micros mainStart() const {
        const model::Range* r = w->selection().resolve(w->selection().snapshot().main);
        return r == nullptr ? 0 : r->start;
    }

    bool quiet() const { return uitest::runtimeQuiet(*rt); }
};

inline std::shared_ptr<const model::DataSource> sourceNamed(widgets::MainWindow& w, const std::string& file) {
    for (const auto& s : w.selection().sources()) {
        if (pathToUtf8(s->prov.path.filename()) == file) {
            return s;
        }
    }
    return nullptr;
}

inline int worksheetNamed(widgets::MainWindow& w, const QString& name) {
    for (int i = 0; i < w.worksheetTabs().count(); ++i) {
        if (w.worksheetTabs().tabText(i) == name) {
            return i;
        }
    }
    return -1;
}

inline QStringList worksheetNames(widgets::MainWindow& w) {
    QStringList out;
    for (int i = 0; i < w.worksheetTabs().count(); ++i) {
        out << w.worksheetTabs().tabText(i);
    }
    return out;
}

// The components of the shown worksheet of type `T`, in paint order.
template <class T> std::vector<T*> componentsOf(widgets::MainWindow& w) {
    std::vector<T*> out;
    for (widgets::ComponentWidget* c : w.canvas().components()) {
        if (auto* t = qobject_cast<T*>(c)) {
            out.push_back(t);
        }
    }
    return out;
}

template <class T> T* componentOf(widgets::MainWindow& w) {
    const std::vector<T*> all = componentsOf<T>(w);
    return all.empty() ? nullptr : all.front();
}

inline QStringList componentTypes(widgets::MainWindow& w) {
    QStringList out;
    for (widgets::ComponentWidget* c : w.canvas().components()) {
        out << QLatin1String(c->type());
    }
    return out;
}

inline void focusComponent(widgets::MainWindow& w, widgets::ComponentWidget* c) {
    w.canvas().focusComponent(c);
    c->setFocus(Qt::OtherFocusReason);
}

// A key in the keymap's portable text ("Ctrl+Alt+F", "Shift+F4", "Return") to the focus widget, as the
// platform sends it (through the KeyEventRouter and the KeymapBinder).
inline void key(widgets::MainWindow& w, const QString& portable) {
    QString t = portable;
    if (t.endsWith(QLatin1String("Delete"))) {
        t.chop(3); // "Del": Qt's portable name
    }
    const QKeyCombination c = QKeySequence::fromString(t, QKeySequence::PortableText)[0];
    Qt::KeyboardModifiers mods = c.keyboardModifiers();
    if (c.key() == Qt::Key_Enter) {
        mods |= Qt::KeypadModifier;
    }
    uitest::press(w, c.key(), mods);
}

// The numbers the text runs of a DrawList show ("+12.3", "245.07", "6").
inline std::vector<double> numbersShown(const plot::DrawList& list) {
    std::vector<double> out;
    for (const plot::TextRun& r : list.texts) {
        const QString text = QString::fromStdString(list.text.substr(r.byteOffset, r.byteLen)).trimmed();
        bool ok = false;
        const double v = text.section(QLatin1Char(' '), 0, 0).toDouble(&ok);
        if (ok) {
            out.push_back(v);
        }
    }
    return out;
}

inline bool shows(const std::vector<double>& numbers, double want, double tolerance) {
    return std::any_of(numbers.begin(), numbers.end(),
                       [&](double v) { return std::abs(v - want) <= tolerance; });
}

inline QString listed(const std::vector<double>& numbers) {
    QStringList s;
    for (const double v : numbers) {
        s << QString::number(v);
    }
    return s.join(QStringLiteral(", "));
}

// The value (base unit) of a uniform series at absolute time t: the sample at or before t. Reads the column,
// which blocks: test code only, under ThreadRole::Normal.
inline std::optional<double> valueAt(const store::Series& series, Micros t) {
    const auto* u = std::get_if<store::UniformSeries>(&series);
    if (u == nullptr || u->size() == 0) {
        return std::nullopt;
    }
    const store::SampleIdx i = std::clamp<store::SampleIdx>(u->indexAt(t), 0, u->size() - 1);
    const ThreadRole::Scope blocking(ThreadRole::Normal);
    auto h = u->values.require(i, i, Priority::Visible, CancelToken{});
    if (!h || !h->validAt(i)) {
        return std::nullopt;
    }
    return h->at(i);
}

inline const model::Channel* channelOf(const model::DataSource& s, std::string_view name) {
    const model::DataLayer* l = s.layer(model::kNormalLayer);
    return l == nullptr ? nullptr : l->find(name);
}

inline std::size_t filesIn(const std::filesystem::path& dir, const std::string& prefix = {}) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        n += e.is_regular_file() && pathToUtf8(e.path().filename()).rfind(prefix, 0) == 0 ? 1 : 0;
    }
    return n;
}

inline std::string readAll(const std::filesystem::path& p) {
    Result<std::string> r = readWholeFile(p);
    return r ? std::move(r).value() : std::string();
}

// WCAG 2 relative luminance and contrast ratio of two "#rrggbb" colours.
inline double luminance(const QColor& c) {
    const auto f = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(static_cast<double>(c.redF())) + 0.7152 * f(static_cast<double>(c.greenF())) +
           0.0722 * f(static_cast<double>(c.blueF()));
}
inline double contrast(const QString& a, const QString& b) {
    const double la = luminance(QColor(a));
    const double lb = luminance(QColor(b));
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace esses::saturday
