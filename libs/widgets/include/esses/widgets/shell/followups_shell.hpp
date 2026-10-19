// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FollowupsShell (M4 followups package; D-206; docs/milestones/M3.md section 6 items 18, 19, 21-27, 29-32,
// mapped in docs/milestones/M4.md section 5): the shell wiring the M3 follow-ups need. As built (D-211 (g)):
// when the loader's summary pass of a source has run (SourceLoader::summariesReady, D-210 (b)), every
// component of the window, shown or kept hidden, gets onDataChanged(source, 0), so one built while the
// source's SummaryPyramids were incomplete rebuilds its extents (M3 item 18; a component that does not show
// the source ignores it). The other follow-ups need no shell wiring: the components and dialogs carry them
// (the private cursor and zoom of an unlinked component, D-211 (f); the Venue edit's Track Sections pass,
// D-210 (d)).
#pragma once

#include <esses/widgets/shell/shell_extension.hpp>

#include <QMetaObject>

namespace esses::widgets {

class FollowupsShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "followups";

    explicit FollowupsShell(MainWindow& window);
    ~FollowupsShell() override;

    void windowReady() override; // connects SourceLoader::summariesReady
    void teardown() override;    // disconnects it

    // The components the last summariesReady reached (tests).
    int lastSummaryDeliveries() const noexcept { return lastDeliveries_; }

private:
    QMetaObject::Connection summaries_;
    int lastDeliveries_ = 0;
};

} // namespace esses::widgets
