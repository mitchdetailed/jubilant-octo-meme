// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AboutShell (about_shell.hpp): the M4 packaging package's shell wiring (wave 2; docs/milestones/M4.md
// section 3): Help > About opens the AboutDialog window-modal. Final apart from the dialog's texts.

#include "main_window_impl.hpp"

#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/about_shell.hpp>

#include <utility>

namespace esses::widgets {

AboutShell::AboutShell(MainWindow& window) : ShellExtension(window, kKey) {}

AboutShell::~AboutShell() = default;

AboutDialog* AboutShell::dialog() const {
    return dialog_.data();
}

void AboutShell::registerCommands() {
    qt::CommandSpec s;
    s.id = QString::fromLatin1(kCommand);
    s.title = tr("About %1...").arg(buildInfo().appName);
    s.menuPath = QStringLiteral("Help");
    s.context = qt::CommandContext::Global;
    s.invoke = [self = QPointer<AboutShell>(this)](const QVariantList&) {
        if (self.isNull()) {
            return;
        }
        if (!self->dialog_.isNull()) {
            self->dialog_->raise();
            return;
        }
        auto* dialog = new AboutDialog(&self->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        self->dialog_ = dialog;
        dialog->open();
    };
    window().commands().add(std::move(s));
}

void AboutShell::teardown() {
    delete dialog_.data(); // a child of the window: gone before the window's widgets
}

} // namespace esses::widgets
