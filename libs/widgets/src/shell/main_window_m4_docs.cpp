// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DocsShell (docs_shell.hpp): the M4 docs package's shell wiring (wave 2; docs/milestones/M4.md section 3).
// The M4-STUB bodies below are replaced here; the window's state is reached with `auto& d = impl();`.

#include "main_window_impl.hpp"

#include <esses/widgets/shell/docs_shell.hpp>

namespace esses::widgets {

DocsShell::DocsShell(MainWindow& window) : ShellExtension(window, kKey) {}

DocsShell::~DocsShell() = default;

void DocsShell::setUrlOpener(UrlOpener /*opener*/) {
    // M4-STUB (docs)
}

QString DocsShell::contentsUrl() const {
    return {}; // M4-STUB (docs)
}

void DocsShell::registerCommands() {
    // M4-STUB (docs): kCommand, once implemented (until then the keymap row stays planned for M4)
}

} // namespace esses::widgets
