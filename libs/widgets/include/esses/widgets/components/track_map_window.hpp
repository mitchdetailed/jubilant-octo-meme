// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapWindow (06 section 9; 04 section 14 item 7; D-027; 07 section 6.1): the content of the Track Map
// dock (DockHost id "trackmap", AutoHide on the right by default). It embeds a TrackPosition whose links
// follow the active worksheet: setContext() rebuilds it over the new worksheet's context (links forced on, 04
// section 14 item 7), keeping the dock's own display settings (its body: the zoom, pan and section toggles of
// the dock, not a workbook document, so they apply at once and are never undo commands). Main dot in
// roles.cursorMain, overlay dots in key colours (independent positions only in time mode), the zoom span,
// optional section labels; PLUS/MINUS and the wheel zoom while focused, drag pans, double-click fits (the
// embedded widget, which takes the keys itself in a dock). Context menu (also on the window's actions()):
// Fit, Show Sections (colours and marks), Show Section Labels, Track Editor..., Regenerate Track... (the last
// two emit editorRequested / regenerateRequested for the shell). Without a context the window shows a "No
// worksheet" note. The track map is auto-generated on open when absent and the required channels exist: the
// shell's job, through TrackEditor's generation path.
#pragma once

#include <esses/project/component_spec.hpp>

#include <QWidget>

class QAction;
class QLabel;
class QVBoxLayout;

namespace esses::widgets {

struct WorksheetContext;
class TrackPosition;

class TrackMapWindow final : public QWidget {
    Q_OBJECT
public:
    explicit TrackMapWindow(QWidget* parent = nullptr);
    ~TrackMapWindow() override;

    // The active worksheet's context (null: none; the window shows the note). Not owned.
    void setContext(WorksheetContext* context);
    WorksheetContext* context() const noexcept { return context_; }

    // The embedded map (null without a context).
    TrackPosition* trackPosition() const noexcept { return position_; }
    // The dock's track_position spec (its body holds the dock's display settings).
    const project::ComponentSpec& spec() const noexcept { return spec_; }
    void fit();
    void setShowSections(bool shown);
    void setShowSectionLabels(bool shown);

    QAction* fitAction() const noexcept { return fit_; }
    QAction* showSectionsAction() const noexcept { return sections_; }
    QAction* showSectionLabelsAction() const noexcept { return labels_; }
    QAction* editorAction() const noexcept { return editor_; }
    QAction* regenerateAction() const noexcept { return regenerate_; }

Q_SIGNALS:
    void editorRequested();
    void regenerateRequested();

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void applySpec(const project::ComponentSpec& spec);
    void syncActions();

    WorksheetContext* context_ = nullptr;
    project::ComponentSpec spec_;
    TrackPosition* position_ = nullptr;
    QLabel* note_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    QAction* fit_ = nullptr;
    QAction* sections_ = nullptr;
    QAction* labels_ = nullptr;
    QAction* editor_ = nullptr;
    QAction* regenerate_ = nullptr;
};

} // namespace esses::widgets
