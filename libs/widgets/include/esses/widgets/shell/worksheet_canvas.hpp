// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetCanvas (06 section 4.3, 04 section 7.2 items 1 and 7): hosts the components of one worksheet at
// their fractional rectangles [l,t,r,b] (0..1 of the content rect, rounded to logical pixels exactly as
// qt::OffscreenRenderer places them; overlap allowed; paint order by `z`, array order breaks ties), routes
// focus (exactly one focused component per worksheet, shown by its Chrome border; a click or keyboard focus
// inside a component focuses it; TAB/SHIFT+TAB cycle in z order, then reading order; the focus is
// remembered per worksheet for the session and returnFocus() gives it back after a dock, 06 section 4.3
// item 13), maximises and restores a component (caption double-click, not persisted), coalesces relayouts
// to 30 Hz (the first resize of a burst lays out at once, the final size always lands) and is the worksheet
// hotkey scope. DPR changes reach the components through Qt (QEvent::DevicePixelRatioChange, handled by
// ComponentWidget as Size|Scheme); the canvas relayouts and repaints its background.
// Worksheet switches (04 section 6.2, D-058): the components of a worksheet that is switched away from are
// hidden, not destroyed; switching back to it (same key, same definition, same context) shows the same
// widgets again, so a quick flip reuses their Static images (a hidden component releases its Static image
// after 30 s). clear() is the only destroyer of every worksheet's components (workbook or project change).
// Edits (M1, no undo stack before M2): a component's specEditRequested is applied here (the worksheet
// definition and the component's applySpec) and reported by componentEdited, from which the shell updates
// its workbook; M2 wraps the same path in a QUndoCommand (06 section 4.3 item 11).
//
// M3 (06 section 4.3 items 2-5, 8-11; 04 section 7.2 items 6-7; AC-M3-11):
//   - Showing the shown worksheet again with another definition (undo/redo, a paste, a delete, an edit from
//     elsewhere) updates it in place: components are matched by id, a changed spec is applied with applySpec
//     (and announced by componentSpecApplied), new ones are created and removed ones destroyed, so the
//     surviving widgets (and a PropertiesDialog tracking one) stay.
//   - Selection: a click selects the component (and focuses it), CTRL+click toggles membership, a rubber
//     band dragged on empty canvas selects by intersection, selectAll(); selected components carry a thin
//     accent frame drawn by an overlay above them (the focus keeps its thick Chrome border).
//   - Move and resize: drag the caption bar, or SHIFT+drag anywhere, moves the selection; dragging an edge or
//     corner (6 logical px zone; a plain press on a control the component draws there - a scrollbar, a
//     handle, the offset axis, the outing's zoom rectangle - stays the component's) resizes one component;
//     both snap to the worksheet grid (1 % by default, WorksheetDef::grid) unless ALT is held, keep at least
//     4 x 4 grid cells and stay inside the canvas; ALT+arrow keys nudge the selection one grid cell (when no
//     keymap row takes the key). Splitters: CTRL+drag on an edge moves that edge for every component touching
//     it.
//   - Every move, resize, splitter drag, nudge and z-order change is one layoutEdited(text, mergeKey) after
//     the definition (worksheet()) was updated; the shell makes it a WorkbookCommand that merges consecutive
//     geometry edits of the same component (mergeKey = its id).
//   - Layout lock (project.json ui.layoutLocked): no move, resize, splitter, nudge or z-order change; the
//     shell refuses delete, cut and paste. Properties and channel edits stay allowed.
//   - Z order: sendToBackground / bringToFront set z = min - 1 / max + 1 and renumber the worksheet 0..n-1.
#pragma once

#include <esses/base/uuid.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QHash>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>
#include <map>
#include <memory>
#include <vector>

class QTimer;

namespace esses::widgets {

class WorksheetCanvas final : public QWidget {
    Q_OBJECT
public:
    explicit WorksheetCanvas(QWidget* parent = nullptr);
    ~WorksheetCanvas() override;

    // Shows the worksheet's components (the context must outlive the canvas content, i.e. until clear()).
    // The active worksheet's components are hidden and kept; a worksheet kept from an earlier call with the
    // same key (id, else name), an equal definition and the same context gets its kept components back,
    // otherwise its components are created (and kept ones of that key destroyed). The focused component is
    // the one the worksheet had, else the one remembered for it (by id), else the first in TAB order; it
    // takes the keyboard focus only when the keyboard focus was inside the canvas. Showing the worksheet that
    // is already shown (same key, definition and context) changes nothing; the shown worksheet with another
    // definition is updated in place (M3, above).
    void setWorksheet(const project::WorksheetDef& worksheet, WorksheetContext& context);
    // Destroys the components of every worksheet, shown or kept.
    void clear();
    const project::WorksheetDef& worksheet() const noexcept { return worksheet_; }
    // Worksheets whose components are kept hidden (tests: D-058 reuse).
    int keptWorksheetCount() const noexcept { return static_cast<int>(kept_.size()); }

    // Paint order (ascending z, stable).
    QList<ComponentWidget*> components() const;
    // TAB order: z, then reading order (top, then left).
    QList<ComponentWidget*> focusOrder() const;
    ComponentWidget* focusedComponent() const;
    ComponentWidget* componentById(const Uuid& id) const;
    // Makes `component` the focused component and gives it the keyboard focus (click, TAB, ESC from a dock).
    void focusComponent(ComponentWidget* component);
    void focusNext(bool backwards = false); // TAB / SHIFT+TAB
    void returnFocus();                     // ESC from a dock: keyboard focus back to the focused component
    void toggleMaximise(ComponentWidget* component);
    ComponentWidget* maximisedComponent() const;
    bool restoreMaximised(); // ESC / the same action; false when nothing was maximised
    // Re-applies a scheme to the canvas background and every component (Options > Colours).
    void applyScheme(const project::Scheme& scheme);

    // The logical rectangle of a fractional rectangle inside `content` (the OffscreenRenderer rounding).
    static QRect placement(const project::Rect& rect, const QRect& content);
    // Relayouts performed so far (tests: the 30 Hz coalescing).
    int layoutPasses() const noexcept { return layoutPasses_; }
    // True while the 30 Hz relayout throttle runs (a resize now would be coalesced, not laid out at once).
    bool layoutThrottleActive() const;
    static constexpr int kLayoutCoalesceMs = 33; // 30 Hz

    // ---- M3 (06 section 4.3 items 2-5, 8-11) ----------------------------------------------------------
    static constexpr int kEdgeZonePx = 6; // resize hit zone at DPR 1 (logical px)
    static constexpr int kMinCells = 4;   // minimum component size in grid cells
    static constexpr int kDragThresholdPx = 3;

    // The explicit selection (paint order); empty when only the focused component is "selected".
    QList<ComponentWidget*> selection() const;
    // What cut/copy/delete act on: the selection, else the focused component.
    QList<ComponentWidget*> selectedOrFocused() const;
    bool isSelected(const ComponentWidget* component) const;
    void setSelection(const QList<ComponentWidget*>& components);
    void toggleSelected(ComponentWidget* component);
    void selectAll(); // component.select_all
    void clearSelection();
    // Selects the shown components with these ids (after a paste).
    void selectIds(const std::vector<Uuid>& ids);

    void setLayoutLocked(bool locked);
    bool layoutLocked() const noexcept { return locked_; }
    // The worksheet grid (columns, rows); 100 x 100 when the definition holds nonsense.
    QSize grid() const;
    // `rect` snapped to `grid` cells (edges rounded to the nearest cell), at least kMinCells x kMinCells and
    // inside [0, 1]; `keepSize` moves without resizing (the width and height in cells are kept).
    static project::Rect snapped(const project::Rect& rect, QSize grid, bool keepSize);

    // The edits, each one layoutEdited (false and nothing changed when locked or a no-op). `rects` maps
    // component ids to their new fractional rectangles (not snapped here).
    bool setRects(const std::vector<std::pair<Uuid, project::Rect>>& rects, const QString& undoText);
    // Moves the selection (else the focused component) by whole grid cells.
    bool nudgeSelection(int dxCells, int dyCells);
    // The splitter of 06 section 4.3 item 8: moves the vertical (true) or horizontal edge at fraction `from`
    // to `to` (snapped) for every component whose edge lies there.
    bool moveSharedEdge(bool vertical, double from, double to, bool snap = true);
    bool sendToBackground(ComponentWidget* component);
    bool bringToFront(ComponentWidget* component);

Q_SIGNALS:
    void focusedComponentChanged(esses::widgets::ComponentWidget* component);
    // A component edit was applied to the worksheet definition (M1: no undo stack).
    void componentEdited(const esses::project::ComponentSpec& spec, const QString& undoText);
    // M3: a spec was applied to a shown component (an edit, or a definition change from undo/redo).
    void componentSpecApplied(esses::widgets::ComponentWidget* component);
    // M3: geometry or z order of components changed in worksheet(); `mergeKey` is the id of the one
    // component a move/resize concerned ("" for a multi-component edit).
    void layoutEdited(const QString& undoText, const QString& mergeKey);
    void selectionChanged();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct KeptWorksheet { // a worksheet switched away from: its components, hidden
        project::WorksheetDef worksheet;
        const WorksheetContext* context = nullptr;
        std::vector<QPointer<ComponentWidget>> components; // paint order
        QPointer<ComponentWidget> focused;
        QPointer<ComponentWidget> maximised;
    };
    struct Gesture; // a mouse move / resize / splitter / rubber band in progress
    class Overlay;

    void keepActive(); // hides the active worksheet's components and moves them to kept_
    void createComponents(WorksheetContext& context);
    ComponentWidget* createComponent(const project::ComponentSpec& spec, WorksheetContext& context);
    void syncInPlace(const project::WorksheetDef& worksheet, WorksheetContext& context);
    void sortAndRaise();
    void layoutComponents();
    void queueLayout();
    void applyEdit(ComponentWidget* component, const project::ComponentSpec& spec, const QString& undoText);
    void
    selectFocused(ComponentWidget* component); // the worksheet's focused component, keyboard focus untouched
    ComponentWidget* componentContaining(QWidget* widget) const;
    void onApplicationFocusChanged(QWidget* old, QWidget* now);
    // Mouse gestures (the application event filter on the canvas's descendants).
    bool mousePress(QWidget* receiver, QMouseEvent* event);
    bool mouseMove(QMouseEvent* event);
    bool mouseRelease(QMouseEvent* event);
    void finishGesture(bool commit);
    QPointF fraction(QPointF canvasPos) const;
    void pruneSelection();
    project::ComponentSpec* specOf(const Uuid& id);

    project::WorksheetDef worksheet_;
    const WorksheetContext* context_ = nullptr; // of the shown worksheet; nullptr: none shown
    QString activeKey_;
    std::map<QString, KeptWorksheet> kept_; // worksheet key -> its hidden components
    std::shared_ptr<const project::Scheme> scheme_;
    std::vector<QPointer<ComponentWidget>> components_; // paint order
    QPointer<ComponentWidget> focused_;
    QPointer<ComponentWidget> maximised_;
    QHash<QString, Uuid> rememberedFocus_; // worksheet key -> component id (this session)
    QTimer* layoutTimer_ = nullptr;
    bool layoutPending_ = false;
    int layoutPasses_ = 0;
    // M3
    std::vector<QPointer<ComponentWidget>> selection_;
    QPointer<ComponentWidget> priorFocused_; // the focused component before the last focus change
    bool locked_ = false;
    std::unique_ptr<Gesture> gesture_;
    Overlay* overlay_ = nullptr;
};

} // namespace esses::widgets
