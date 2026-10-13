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
// its workbook; M2 wraps the same path in a QUndoCommand (06 section 4.3 item 11). Move/resize/snap,
// multi-select, splitters and the clipboard follow in M3 with the layout tools.
#pragma once

#include <esses/base/uuid.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QRect>
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
    // is already shown (same key, definition and context) changes nothing.
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

Q_SIGNALS:
    void focusedComponentChanged(esses::widgets::ComponentWidget* component);
    // A component edit was applied to the worksheet definition (M1: no undo stack).
    void componentEdited(const esses::project::ComponentSpec& spec, const QString& undoText);

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct KeptWorksheet { // a worksheet switched away from: its components, hidden
        project::WorksheetDef worksheet;
        const WorksheetContext* context = nullptr;
        std::vector<QPointer<ComponentWidget>> components; // paint order
        QPointer<ComponentWidget> focused;
        QPointer<ComponentWidget> maximised;
    };

    void keepActive(); // hides the active worksheet's components and moves them to kept_
    void createComponents(WorksheetContext& context);
    void layoutComponents();
    void queueLayout();
    void applyEdit(ComponentWidget* component, const project::ComponentSpec& spec, const QString& undoText);
    void
    selectFocused(ComponentWidget* component); // the worksheet's focused component, keyboard focus untouched
    ComponentWidget* componentContaining(QWidget* widget) const;
    void onApplicationFocusChanged(QWidget* old, QWidget* now);

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
};

} // namespace esses::widgets
