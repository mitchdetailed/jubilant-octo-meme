// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BudgetShell (budget_shell.hpp): the M4 budget package's shell wiring (docs/milestones/M4.md section 3; 07
// section 7.1 row 1, AC-M4-02, demo 7.5 line 3; D-206 (c)-(e), D-207). The window's state is reached with
// `auto& d = impl();`; the Performance field added to d.status is found by MainWindow::statusField and is
// cleared on every worksheet switch (MainWindow::selectWorksheet), so its text is set again in
// worksheetActivated.

#include "main_window_impl.hpp"

#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/dock_host.hpp>

#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QStatusBar>
#include <algorithm>
#include <utility>

namespace esses::widgets {

BudgetShell::BudgetShell(MainWindow& window) : ShellExtension(window, kKey) {}

BudgetShell::~BudgetShell() = default;

qt::PerformanceMonitor* BudgetShell::monitor() const {
    return monitor_.data();
}

QLabel* BudgetShell::performanceLabel() const {
    return label_.data();
}

void BudgetShell::buildWidgets(QWidget& /*central*/) {
    auto& d = impl();
    QStatusBar* bar = window().statusBar();
    auto* label = new QLabel(bar);
    label->setObjectName(QStringLiteral("status.") + QLatin1String(kStatusField));
    label->setAccessibleName(tr("Memory budget"));
    label->setCursor(Qt::PointingHandCursor);
    label->installEventFilter(this); // a click shows Messages (06 section 2.1)
    bar->addPermanentWidget(label);
    d.status.emplace(QString::fromLatin1(kStatusField), label);
    label_ = label;

    monitor_ = new qt::PerformanceMonitor(d.runtime, this);
    connect(monitor_.data(), &qt::PerformanceMonitor::updated, this,
            [this](const qt::PerformanceSnapshot& s) { showSnapshot(s); });
}

void BudgetShell::registerCommands() {
    const QPointer<BudgetShell> self(this);
    const auto add = [&](const char* id, const QString& title, bool proceed) {
        qt::CommandSpec s;
        s.id = QString::fromLatin1(id);
        s.title = title;
        // No menu and no keymap row (D-206 (h)): the Messages row's links and the command palette run them.
        s.context = qt::CommandContext::Global;
        s.enabled = [self] { return !self.isNull() && self->window().loader().pendingBudgetOpens() > 0; };
        s.invoke = [self, proceed](const QVariantList& args) {
            if (!self.isNull()) {
                self->answer(args, proceed);
            }
        };
        window().commands().add(std::move(s));
    };
    add(kContinueCommand, tr("Continue Opening"), true);
    add(kCancelCommand, tr("Cancel Opening"), false);
}

void BudgetShell::windowReady() {
    auto& d = impl();
    runtimeBudget_ = d.runtime.cache().budgetBytes();
    applyBudgetSetting();
    connections_.push_back(
        connect(&window().settings(), &qt::SettingsStore::changed, this, [this](const QString& key) {
            if (key == QLatin1String(qt::kBudgetSettingKey)) {
                applyBudgetSetting();
            }
        }));
    qt::SourceLoader& loader = window().loader();
    loader.setBudgetGate(&d.runtime.cache());
    connections_.push_back(
        connect(&loader, &qt::SourceLoader::budgetExceeded, this,
                [this](int pendingId, const qt::OpenBudgetCheck& check) { warn(pendingId, check); }));
    if (monitor_ != nullptr) {
        monitor_->start();
        monitor_->refresh();
    }
}

void BudgetShell::worksheetActivated(int /*index*/) {
    // The switch cleared every status field: the newest snapshot again (empty before the first one).
    if (label_ != nullptr && monitor_ != nullptr) {
        label_->setText(qt::performanceLine(monitor_->last()));
    }
}

void BudgetShell::teardown() {
    if (monitor_ != nullptr) {
        monitor_->stop();
        disconnect(monitor_.data(), nullptr, this, nullptr);
    }
    for (const QMetaObject::Connection& c : connections_) {
        disconnect(c);
    }
    connections_.clear();
    if (label_ != nullptr) {
        label_->removeEventFilter(this);
    }
    auto& d = impl();
    // The loader outlives this extension: it stops asking (the opens still waiting go with the loader).
    if (window().loader().budgetGate() == &d.runtime.cache()) {
        window().loader().setBudgetGate(nullptr);
    }
    // A runtime outlives its window in the tests: its own budget again, unless someone else changed it since.
    if (applied_ && !d.runtime.isShutDown() && d.runtime.cache().budgetBytes() == *applied_) {
        d.runtime.cache().setBudgetBytes(runtimeBudget_);
    }
    applied_.reset();
}

bool BudgetShell::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_.data() && event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        window().docks().show(QString::fromLatin1(MessagesWindow::kDockId));
        return true;
    }
    return ShellExtension::eventFilter(watched, event);
}

void BudgetShell::applyBudgetSetting() {
    auto& d = impl();
    const std::optional<std::size_t> chosen = qt::budgetFromSettings(window().settings());
    const std::size_t budget = chosen.value_or(runtimeBudget_);
    applied_ = chosen;
    if (budget != 0 && d.runtime.cache().budgetBytes() != budget) {
        d.runtime.cache().setBudgetBytes(budget);
    }
    if (monitor_ != nullptr) {
        monitor_->refresh();
    }
}

void BudgetShell::showSnapshot(const qt::PerformanceSnapshot& snapshot) {
    if (label_ != nullptr) {
        label_->setText(qt::performanceLine(snapshot));
        label_->setToolTip(qt::performanceDetails(snapshot) + QLatin1Char('\n') +
                           tr("Click to show the Messages window."));
    }
    window().messagesWindow().showPerformance(snapshot);
}

void BudgetShell::warn(int pendingId, const qt::OpenBudgetCheck& check) {
    qt::Message m;
    m.severity = qt::Message::Severity::Warning;
    m.origin = qt::Message::Origin::Budget;
    m.source = check.path;
    m.code = QString::fromLatin1(kWarningCode);
    m.text = qt::budgetWarningText(check);
    m.action = QString::fromLatin1(kContinueCommand);
    m.actionArgs = {pendingId};
    m.altAction = QString::fromLatin1(kCancelCommand);
    m.altActionArgs = {pendingId};
    warned_.push_back(pendingId);
    window().messages().append(std::move(m));
    window().docks().show(QString::fromLatin1(MessagesWindow::kDockId));
    hint(tr("%1 is above the memory budget: Continue or Cancel in Messages")
             .arg(QFileInfo(check.path).fileName()));
}

std::optional<int> BudgetShell::pendingIdOf(const QVariantList& args) const {
    const qt::SourceLoader& loader = window().loader();
    if (!args.isEmpty()) {
        bool ok = false;
        const int id = args.front().toInt(&ok);
        return ok && loader.pendingBudgetCheck(id) != nullptr ? std::optional<int>(id) : std::nullopt;
    }
    for (auto it = warned_.rbegin(); it != warned_.rend(); ++it) {
        if (loader.pendingBudgetCheck(*it) != nullptr) {
            return *it;
        }
    }
    return std::nullopt;
}

void BudgetShell::answer(const QVariantList& args, bool proceed) {
    const std::optional<int> id = pendingIdOf(args);
    if (!id) {
        hint(tr("That open is no longer waiting"));
        return;
    }
    // Forget the ids that no longer wait (answered, or dropped by a project change).
    qt::SourceLoader& loader = window().loader();
    const int answered = *id;
    warned_.erase(std::remove_if(warned_.begin(), warned_.end(),
                                 [&loader, answered](int w) {
                                     return w == answered || loader.pendingBudgetCheck(w) == nullptr;
                                 }),
                  warned_.end());
    if (proceed) {
        loader.continueOverBudget(answered);
        if (monitor_ != nullptr) {
            monitor_->refresh(); // the Performance line at once, then on every tick as the chunks load
        }
    } else {
        loader.cancelOverBudget(answered);
    }
}

} // namespace esses::widgets
