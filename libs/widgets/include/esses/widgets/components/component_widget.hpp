// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentWidget (04 section 7.1 is the single declaration, 06 section 4.4 the shell's use of it; D-018,
// D-027): the base of every worksheet component. A component is created by its JSON type string through the
// factory (unknown types -> PlaceholderComponent, 06 section 4.3 item 12), never edits its own spec in place
// (it emits specEditRequested and receives applySpec after the command executes), reacts to the view through
// onViewChanged(ViewState, ChangeMask) (Cursor/Datum -> Dynamic only; 04 section 2.2), submits Static builds
// only through RenderService, never blocks, and paints from a qt::LayerCache. Shared behaviours: 04 section
// 7.2 (each with a Qt Test row). String types are Qt types (06 section 1 rule 1).
//
// WorksheetContext (06 section 4.4) gives a component its WorksheetViewModel, DataSelectionModel,
// MathsService, RenderService, scheme and CommandRegistry. Registration: registerBuiltinComponents()
// (components/ builtin_components.cpp) registers the ten MVP types (since the M3 contract); nothing registers
// itself statically (static-library initialisers are dropped by the linker).
//
// Wiring (M1): the base class connects itself to its context, so the shell forwards nothing:
//   WorksheetViewModel::changed        -> onViewChanged(view.state(), mask) without the Size bit (the
//                                         worksheet's Size bit is its cursor-step width; a component's own
//                                         size arrives through resizeEvent)
//   DataSelectionModel::changed        -> onSelectionChanged(snapshot)
//   MathsService::channelReady/statusChanged, DistanceService::distanceReady/noDistance
//                                      -> onDataChanged(source, 0)
//   resizeEvent                        -> onViewChanged(state, Size), coalesced to 30 Hz (04 section 6.3
//                                         item 6): the first change of a burst is delivered at once, later
//                                         ones at most every 33 ms, and the final size always arrives
//   QEvent::DevicePixelRatioChange / ScreenChangeInternal with a new devicePixelRatioF()
//                                      -> onViewChanged(state, Size | Scheme) (full rebuild; 04 section 6.6
//                                         item 3, 06 section 17 item 3)
// Focus (04 section 7.2 item 1): WorksheetCanvas owns "the focused component of the worksheet" and calls
// setFocusedComponent (Chrome border); a click emits focusRequested; a double-click on the caption bar emits
// maximiseRequested (06 section 4.3 item 6).
#pragma once

#include <esses/model/ids.hpp>
#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/project/scheme.hpp>
#include <esses/qtmodel/command_registry.hpp>

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace esses::qt {
class CommandRegistry;
class DataSelectionModel;
class MathsService;
class RenderService;
class WorksheetViewModel;
} // namespace esses::qt

namespace esses::widgets {

class ComponentWidget;

struct WorksheetContext {
    qt::WorksheetViewModel& view;
    qt::DataSelectionModel& selection;
    qt::MathsService& maths;
    qt::RenderService& render;
    qt::CommandRegistry& commands;
    std::shared_ptr<const project::Scheme> scheme;
    QString worksheetId; // build key part (RenderService)
};

struct ExportOptions { // 04 section 7.2 item 8
    enum class Range : std::uint8_t { CurrentZoom, SelectedPadded, AllData };
    Range range = Range::CurrentZoom;
};

class ExportSink { // CSV in MVP
public:
    virtual ~ExportSink() = default;
    virtual bool write(const QByteArray& bytes) = 0;

protected:
    ExportSink() = default;
    ExportSink(const ExportSink&) = default;
    ExportSink& operator=(const ExportSink&) = default;
};

class ComponentWidget : public QWidget {
    Q_OBJECT
public:
    enum class OverlayPolicy : std::uint8_t { MainOnly, MainAndReference, AllOverlays };
    using Factory =
        std::function<ComponentWidget*(const project::ComponentSpec&, WorksheetContext&, QWidget*)>;

    // Factory by type string; unknown types -> PlaceholderComponent. The caller owns the widget (parent).
    static ComponentWidget* create(const project::ComponentSpec& spec, WorksheetContext& context,
                                   QWidget* parent = nullptr);
    static void registerType(const QString& type, Factory factory);
    static bool isRegistered(const QString& type);

    ComponentWidget(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~ComponentWidget() override;

    virtual const char* type() const = 0; // JSON type string
    const project::ComponentSpec& spec() const noexcept { return spec_; }
    virtual void applySpec(const project::ComponentSpec& spec); // after undo/redo or properties Apply
    virtual void onViewChanged(const plot::ViewState& view, plot::ChangeMask what);
    virtual void onSelectionChanged(const plot::SelectionSnapshot& selection);
    virtual void onDataChanged(const model::SourceId& source, std::uint64_t version);
    virtual void onSchemeChanged(const project::Scheme& scheme);
    virtual OverlayPolicy overlayPolicy() const;
    virtual QStringList channelNames() const; // Values window "focused component" rows (06 section 8)
    virtual std::optional<QString> activeChannel() const;
    virtual bool acceptsChannelDrop() const; // Channels window ENTER / drag target
    virtual void addChannel(const QString& name, bool newPanel);
    virtual QStringList contextCommands() const; // command ids of the Component menu / context menu
    virtual bool exportData(ExportSink& sink, const ExportOptions& options) const;
    virtual QImage renderToImage(qreal dpr) const; // Print to Clipboard: current layers, never blocks
    virtual void openProperties(int tabIndex = 0); // F5 / CTRL+N: PropertiesDialog::openFor (M3)
    // Added in M1: the first context of the key chain while this component has the focus (06 section 14.2:
    // Graph for time_distance, Summary/Track for the later types); Component by default.
    virtual qt::CommandContext commandContext() const;
    // Added in M1: what lies under a widget-logical position, from the hit regions kept with the component's
    // Static layer (04 section 5.5: HitTester); mouse input is routed through it. HitKind::None by default
    // and before the first Static build.
    virtual plot::HitResult hitAt(QPointF logical) const;
    // Added in M1 (04 section 8.6): cancels a mouse gesture in progress that ESC ends (the time_distance
    // rubber band). Returns true when there was one; the shell offers ESC here before view.return_focus.
    // False by default.
    virtual bool cancelTransient();
    // Added in M3 (06 section 14.4 "Components general" and the graph, summary and track rows): a command
    // the shell offers the focused component first (zoom.v_in, pan.up, track.zoom_in,
    // graph.copy_cursor_values, component.export_data, ...). True when the component handled it. False by
    // default: the shell then applies its generic behaviour (the body toggles and cycles of L, K, G, S,
    // ALT+n, F3, M, E, SHIFT+E, Y; widgets src/properties_shell/component_commands.cpp) or tells the user the
    // component has no such command.
    virtual bool handleCommand(const QString& id, const QVariantList& args);
    // Added in M3: the caption bar height in logical px (the canvas's move zone, the maximise zone).
    int captionBarHeight() const { return captionHeight(); }

    WorksheetContext& context() noexcept { return context_; }

    // The worksheet's focused component (04 section 7.2 item 1: thick border in Chrome). Set by
    // WorksheetCanvas only; independent of the Qt keyboard focus, which may sit in a dock.
    bool isFocusedComponent() const noexcept { return focusedComponent_; }
    void setFocusedComponent(bool focused);
    // The device pixel ratio of the last delivered Size change (0 before the first).
    qreal layoutDevicePixelRatio() const noexcept { return dpr_; }
    // Size changes delivered to onViewChanged so far (tests: the 30 Hz coalescing).
    int sizeChangesDelivered() const noexcept { return sizeDeliveries_; }
    // True while the 30 Hz size throttle runs (a size change now would be coalesced, not delivered at once).
    bool sizeThrottleActive() const;
    static constexpr int kSizeCoalesceMs = 33; // 30 Hz

Q_SIGNALS:
    void specEditRequested(const esses::project::ComponentSpec& newSpec, const QString& undoText);
    void focusRequested();
    void maximiseRequested(); // double-click on the caption bar (06 section 4.3 item 6)

protected:
    virtual void onFocusChanged(bool focused);
    // Height of the caption bar in logical px (0 when hidden): the double-click-to-maximise zone.
    virtual int captionHeight() const;

    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    project::ComponentSpec spec_;
    WorksheetContext& context_;

private:
    void queueSizeChange(plot::ChangeMask what);
    void flushSizeChange();

    QTimer* sizeTimer_ = nullptr;
    plot::ChangeMask sizePending_ = 0;
    qreal dpr_ = 0.0;
    int sizeDeliveries_ = 0;
    bool focusedComponent_ = false;
};

// Registers the ten MVP component types (04 sections 8-17). Idempotent.
void registerBuiltinComponents();

} // namespace esses::widgets
