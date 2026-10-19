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
// Links (M4, 04 section 2.4, D-211 (f); docs/milestones/M3.md section 6 item 27): a component with
// links.cursor, links.zoom or links.datum off keeps its own cursor, zoom or datum, taken from the worksheet's
// when it is created or the link is switched off and never persisted; domain and offsetAxis stay the
// worksheet's (F9 restarts an own zoom from the worksheet's converted one); the outing graph's zoom is always
// the worksheet's (it is the zoom control). viewState() is what the component shows and is what
// onViewChanged receives: a worksheet change of an unlinked part is not delivered, and the component's own
// view edits (setViewCursor, setViewZoom, viewPanBy, viewZoomFull: its mouse) go to its own state when
// unlinked. While it is the focused component, the worksheet keyboard rows of an unlinked part (cursor.*,
// zoom.in/out/default_range/datum_main, pan.*, datum.*) act on its own state (handleCommand's default),
// through the Main mapping the component supplies (mainMapper; without one, time mode only).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>
#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/domain_mapper.hpp>
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

class QContextMenuEvent;
class QKeyEvent;
class QMenu;
class QTimer;

namespace esses::qt {
class CommandRegistry;
class DataSelectionModel;
class MathsService;
class RenderService;
class WorksheetViewModel;
enum class CursorSource : std::uint8_t;
} // namespace esses::qt

namespace esses::maths {
struct ChannelKey;
} // namespace esses::maths

namespace esses::plot {
struct ComponentBuildInputs;
} // namespace esses::plot

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
    // Added by the M3 fix pass (D-203 (x)): Export Data as a self-contained job (it holds copies of the
    // inputs, never the component) that produces the CSV on a worker; empty when the component exports
    // nothing or only synchronously (exportData). The shell runs it on the pool and writes the file when it
    // completes, so the GUI thread never waits on it (04 section 6.3) and the component may go meanwhile.
    using ExportJob = std::function<std::optional<QByteArray>(const CancelToken&)>;
    virtual ExportJob exportJob(const ExportOptions& options) const;
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
    // component has no such command. M4 (D-211 (f)): the default carries out the worksheet keyboard rows of
    // an unlinked part on the component's own view (see Links above); a subclass falls through to it.
    virtual bool handleCommand(const QString& id, const QVariantList& args);
    // Added in M3: the caption bar height in logical px (the canvas's move zone, the maximise zone).
    int captionBarHeight() const { return captionHeight(); }
    // Added by the M3 fix pass (D-203 (o)): the keys the user's keymap binds to a command, for the keys a
    // component handles itself where no command reaches it (the Track Map dock's map, ALT+n inside a report
    // table without a shell). The shell sets the provider (KeymapBinder::keysFor) and clears it when it
    // goes; without one the defaults are the binding. keyBound: whether `event` is one of those keys, so a
    // key the user unbound or rebound in keymap.json never acts through the component.
    using KeysProvider = std::function<QStringList(const QString& commandId)>;
    static void setKeysProvider(KeysProvider provider);
    static bool keyBound(const QString& commandId, const QKeyEvent& event, const QStringList& defaults);

    WorksheetContext& context() noexcept { return context_; }

    // The worksheet's focused component (04 section 7.2 item 1: thick border in Chrome). Set by
    // WorksheetCanvas only; independent of the Qt keyboard focus, which may sit in a dock.
    bool isFocusedComponent() const noexcept { return focusedComponent_; }
    void setFocusedComponent(bool focused);
    // The device pixel ratio of the last delivered Size change (0 before the first).
    qreal layoutDevicePixelRatio() const noexcept { return dpr_; }
    // M4 (hidpi package, D-206; src/components/m4_hidpi/component_dpr.cpp): re-checks devicePixelRatioF()
    // against layoutDevicePixelRatio() now (DprWatcher calls it for the components of a window whose screen
    // or DPR changed) and, when they differ, queues the Size | Scheme rebuild that QEvent::
    // DevicePixelRatioChange queues; true when it did.
    bool recheckDevicePixelRatio();
    // M4 (04 section 2.4, D-211 (f)): the links of the component's spec (the outing graph's zoom is always
    // linked) and the view it shows (the worksheet's, with its own zoom, cursor or datum where unlinked).
    bool zoomLinked() const noexcept;
    bool cursorLinked() const noexcept { return spec_.links.cursor; }
    bool datumLinked() const noexcept { return spec_.links.datum; }
    plot::ViewState viewState() const;
    // The component's own view edits (its mouse, its commands): the worksheet's when the part is linked
    // (WorksheetViewModel::setCursor, zoomTo, panBy, zoomFull), else its own state, clamped to the Main
    // outing when mainMapper() knows it, followed by onViewChanged(viewState(), the changed bits).
    void setViewCursor(Micros tMainLap, qt::CursorSource source);
    void setViewZoom(plot::XSpan span);
    void viewPanBy(double fractionOfSpan);
    void viewZoomFull();
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
    // Added by the M3 fix pass (D-203 (j)): a maths channel became ready or changed its status
    // (MathsService::channelReady / statusChanged). The default is a data change of its source
    // (onDataChanged(key.src, 0)); a component that knows the channels it draws ignores the others.
    virtual void onChannelDataChanged(const maths::ChannelKey& key);
    // Height of the caption bar in logical px (0 when hidden): the double-click-to-maximise zone.
    virtual int captionHeight() const;

    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Added by the M3 fix pass (D-203 (s)): the Component menu is also the component's context menu (06
    // section 2.2): contextCommands() as the registry titles them, the commands not available now left out,
    // each run on this component (it takes the focus first). commandMenu builds such a menu of any ids (on
    // the heap, deleted when closed; empty when none applies).
    void contextMenuEvent(QContextMenuEvent* event) override;
    QMenu* commandMenu(const QStringList& commandIds);
    // The Main slot's mapping in the current domain, from the component's inputs (alignment offsets
    // excluded); nullopt (the default) when the component has none. The own zoom's limits and the keyboard
    // rows of an unlinked part use it; mainMapperOf builds it from a component's prepared inputs.
    virtual std::optional<plot::DomainMapper> mainMapper() const;
    static std::optional<plot::DomainMapper> mainMapperOf(const plot::ComponentBuildInputs& inputs,
                                                          plot::Domain domain);
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    project::ComponentSpec spec_;
    WorksheetContext& context_;

private:
    void queueSizeChange(plot::ChangeMask what);
    void flushSizeChange();
    // M4 (D-211 (f)): the own view of the unlinked parts.
    struct OwnView {
        plot::XSpan zoom;
        Micros cursorT = 0;
        std::optional<Micros> datumT;
        std::optional<Micros> hiddenDatumT; // D hid the own datum: its value, shown again by D
    };
    void takeOwnView(bool zoom, bool cursor, bool datum); // from the worksheet's current view
    void ownViewChanged(plot::ChangeMask what);
    bool handleOwnViewCommand(const QString& id);
    std::optional<double> ownCursorX(const std::optional<plot::DomainMapper>& map) const;
    std::optional<Micros> ownTimeAt(const std::optional<plot::DomainMapper>& map, double x) const;
    plot::XSpan clampToOuting(plot::XSpan span, const std::optional<plot::DomainMapper>& map) const;
    OwnView own_;
    std::uint64_t ownVersion_ = 0; // own view changes so far (added to the worksheet's version)

    QTimer* sizeTimer_ = nullptr;
    plot::ChangeMask sizePending_ = 0;
    qreal dpr_ = 0.0;
    int sizeDeliveries_ = 0;
    bool focusedComponent_ = false;
};

// Registers the ten MVP component types (04 sections 8-17). Idempotent.
void registerBuiltinComponents();

} // namespace esses::widgets
