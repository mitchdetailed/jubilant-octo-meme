// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The one M4 hook of MainWindow (D-206; docs/milestones/M4.md sections 2-3; shell_extension.hpp): the
// ShellExtension base and the list of the M4 packages' extensions, created in a fixed order. Contract file:
// the packages edit their own src/shell/main_window_m4_<package>.cpp, never this one.

#include "main_window_impl.hpp"

#include <esses/widgets/shell/about_shell.hpp>
#include <esses/widgets/shell/budget_shell.hpp>
#include <esses/widgets/shell/docs_shell.hpp>
#include <esses/widgets/shell/followups_shell.hpp>
#include <esses/widgets/shell/hidpi_shell.hpp>
#include <esses/widgets/shell/reload_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

namespace esses::widgets {

ShellExtension::ShellExtension(MainWindow& window, const char* key)
    : QObject(&window), window_(window), key_(QString::fromLatin1(key)) {
    setObjectName(QStringLiteral("m4.") + key_);
}

ShellExtension::~ShellExtension() = default;

void ShellExtension::buildWidgets(QWidget& /*central*/) {}
void ShellExtension::registerCommands() {}
void ShellExtension::buildMenus() {}
void ShellExtension::windowReady() {}
void ShellExtension::worksheetActivated(int /*index*/) {}
void ShellExtension::teardown() {}

MainWindow::Impl& ShellExtension::impl() const {
    return *window_.impl_;
}

void ShellExtension::hint(const QString& text) const {
    window_.hint(text);
}

void ShellExtension::showEditor(QDialog& dialog, const char* geometryKey) const {
    window_.showEditor(dialog, geometryKey);
}

void MainWindow::createM4Extensions() {
    // The contract's order (D-206): wave 1 budget, reload, hidpi, followups; wave 2 about, docs. Each hook
    // runs them in this order; teardown runs them in this order too, and they are deleted in the reverse one.
    impl_->extensions = {new BudgetShell(*this),    new ReloadShell(*this), new HiDpiShell(*this),
                         new FollowupsShell(*this), new AboutShell(*this),  new DocsShell(*this)};
}

void MainWindow::forEachM4Extension(const std::function<void(ShellExtension&)>& run) {
    for (ShellExtension* e : impl_->extensions) {
        run(*e);
    }
}

void MainWindow::destroyM4Extensions() {
    Impl& d = *impl_;
    for (ShellExtension* e : d.extensions) {
        e->teardown();
    }
    for (auto it = d.extensions.rbegin(); it != d.extensions.rend(); ++it) {
        delete *it;
    }
    d.extensions.clear();
}

} // namespace esses::widgets
