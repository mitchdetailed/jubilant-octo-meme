// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FollowupsShell (followups_shell.hpp): the M4 followups' shell wiring (docs/milestones/M4.md sections 3 and
// 5; D-211 (g)).

#include "main_window_impl.hpp"

#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/followups_shell.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

namespace esses::widgets {

FollowupsShell::FollowupsShell(MainWindow& window) : ShellExtension(window, kKey) {}

FollowupsShell::~FollowupsShell() = default;

void FollowupsShell::windowReady() {
    // M3 item 18: the summary pass of a source has run, so its SummaryPyramids exist; every component (the
    // kept, hidden ones too, which catch up when shown) takes it as a data change of that source.
    summaries_ = connect(&window().loader(), &qt::SourceLoader::summariesReady, this,
                         [this](const model::SourceId& source) {
                             auto& d = impl();
                             lastDeliveries_ = 0;
                             if (d.canvas == nullptr) {
                                 return;
                             }
                             for (ComponentWidget* c : d.canvas->findChildren<ComponentWidget*>()) {
                                 c->onDataChanged(source, 0);
                                 ++lastDeliveries_;
                             }
                         });
}

void FollowupsShell::teardown() {
    disconnect(summaries_);
}

} // namespace esses::widgets
