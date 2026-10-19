// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceGraph, JSON type "time_distance" (04 section 8, parity-first section 5.4; AC-M1-01, -02, -04,
// -05, -09; AC-M3-02, -05, -09). The layers come from plot::TimeDistanceBuilder (the same builders as
// OffscreenRenderer, so a Print to Clipboard equals esses-render), painted by a qt::LayerCache:
//   Static   a worker build through RenderService::submitBuild (replace key trace/<worksheet>/<component>,
//            Priority::Visible; TraceBuilder::decimateMany and the trace emission fan out on the TaskPool),
//            rasterised by the same worker (RasterBackend::rasteriseStatic, parallel strips; 04 section 6.3
//            step 2 "worker build + raster") and delivered on the GUI thread through the Dispatcher, which
//            only stores the image (LayerCache::storeStatic; LayerCache::rasteriseStatic when the worker had
//            no image, e.g. a cancelled raster). Every request prepares its inputs with
//            RenderService::prepareInputs (the current ViewState, SelectionSnapshot, device size and DPR),
//            then applies the WorksheetViewModel's pending alignment offsets (a drag moves the traces at
//            once, 04 section 3.3 item 4) and the project's channel override limits (scale lines min / max,
//            Channel scaling). Requests are coalesced to one per event-loop turn; a delivered build is always
//            shown (a superseded one as an interim frame, 04 section 6.3 step 3). Each build also reports
//            what it placed (plot::TdStaticInfo: band entries, markers, measurements, scales, the scrollbar
//            extent).
//   Dynamic  built synchronously on the GUI thread on Cursor/Datum (and drag feedback), painted under
//            LayerCache::dynamicDamage: a cursor move never submits a Static build and never repaints the
//            rest of the component (04 section 6.2, AC-M1-02) - except when the datum appears or disappears,
//            which changes the key columns (value@datum and delta). Key values still waiting for a chunk
//            ("...") are retried every 50 ms. Widget-local Dynamic additions: the rubber band, the
//            measurement hover line, the Lo/Hi and splitter drag guides.
//   Chrome   caption, focus border, active-channel square, the pending stripe and the banners.
// Behaviours (04 sections 8.2-8.13; the commands reach handleCommand, 06 section 14.4):
//   F3 graph.variance, M graph.measurements, E graph.status_errors, SHIFT+E graph.device_events, Y
//   cursor.style, G component.cycle_layout (panels <-> overlapped), S component.cycle_style (line <->
//   points): body edits (specEditRequested, undoable); CTRL+SHIFT+C graph.copy_cursor_values (name, value,
//   unit rows at the Main cursor; with a datum value@main, value@datum and delta); Export Data (CSV of the
//   graph's channels in the Main range over the zoom); the marker band's annotation Edit / Remove (undoable
//   sidecar edits through DataSelectionModel::commitSidecar; Add Annotation is the shell's
//   cursor.add_annotation); ALT+UP / ALT+DOWN / ALT+F2 and SHIFT+UP / SHIFT+DOWN: the active group's vertical
//   zoom and pan (view state local to the component, not persisted); the mouse rows' programmatic forms
//   (zoom.rubber_band, zoom.full_axis, zoom.wheel, pan.drag_axis, cursor.place, offset.drag). The offset keys
//   (CTRL+SHIFT / ALT+SHIFT + arrows) are the WorksheetViewModel's: the graph reports the sample period of
//   the active channel in every visible slot's source (WorksheetViewModel::setSourceSamplePeriod), so a range
//   moves by one sample period of its own source, or 0.25 m in distance mode (AC-M3-09).
// Mouse (routed through hitAt, the HitTester delivered with the Static layer): a click or drag in a panel
// places the cursor (snapped to the nearest sample of the active channel in the Main slot when cursorSnap);
// the wheel zooms x0.8 / x1.25 about the pointer; dragging the x axis or the scrollbar thumb pans, a click on
// the scrollbar track pages, a double-click on a scrollbar is full zoom on that axis; dragging a y axis or
// the vertical scrollbar pans vertically; a double-click in a panel starts a rubber band (ALT vertical on the
// active group, CTRL a box) that the next click turns into a zoom (ESC cancels); a double-click on a range or
// section band entry zooms to it (clicks there never move the cursor); dragging an offset sub-axis offsets
// that slot (one undo command at the release); dragging a Lo/Hi handle (overlapped) or a panel splitter edits
// the groups at the release; hovering a min/max measurement draws its position line and a double-click moves
// the cursor there; tooltips name markers, annotations, device events and status rows; right-click an
// annotation: Edit / Remove. While it is the worksheet's focused component it reports its trace width
// (WorksheetViewModel::setSize, cursor steps) and its active channel's sample period (setActiveSamplePeriod,
// zoom minimum). Like-units grouping (04 section 8.2 item 3): addChannel joins the first group whose quantity
// equals the channel's; newPanel (CTRL) or no match appends a new group named after the channel. Memory (04
// section 6.2): a component hidden for 30 s releases its Static image; showing it rebuilds.
// M4 (04 section 8.2 item 4, D-211 (e)): a group's heading is its part of the key
// column outside its labels (plot::HitKind::GroupHeading, an open-hand pointer over it); dragging it shows
// where the group goes (a dashed line between the groups' keys) and the release moves the group there (one
// undoable edit, moveGroup: the group takes its traces, vertical zoom and unknown members along; panels with
// explicit edges keep their heights); a double-click opens Group Properties (the Properties dialog at that
// group's record of the Channels tab: name, scale, limits, lines, Lo/Hi; openGroupProperties). Links (M4,
// D-211 (f)): the zoom, cursor and datum are viewState()'s; an unlinked graph's mouse and keys move its own.
#pragma once

#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QRegion>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QTimer;

namespace esses::widgets {

class PropertiesDialog;

class TimeDistanceGraph final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "time_distance";
    static constexpr int kPendingStripeDelayMs = 100; // the stripe shows only for a build that takes longer
    static constexpr int kValueRetryMs = 50;          // "..." key values waiting for a chunk
    static constexpr int kReleaseHiddenMs = 30'000;   // 04 section 6.2 memory rule (D-058)
    static constexpr double kVerticalZoomStep = 0.5;  // ALT+UP halves the shown fraction, ALT+DOWN doubles it
    static constexpr double kVerticalPanStep = 0.1;   // SHIFT+UP/DOWN: a tenth of the shown span

    TimeDistanceGraph(const project::ComponentSpec& spec, WorksheetContext& context,
                      QWidget* parent = nullptr);
    ~TimeDistanceGraph() override;

    const char* type() const override { return kType; }
    void applySpec(const project::ComponentSpec& spec) override;
    void onViewChanged(const plot::ViewState& view, plot::ChangeMask what) override;
    void onSelectionChanged(const plot::SelectionSnapshot& selection) override;
    void onDataChanged(const model::SourceId& source, std::uint64_t version) override;
    void onSchemeChanged(const project::Scheme& scheme) override;
    OverlayPolicy overlayPolicy() const override { return OverlayPolicy::AllOverlays; }
    qt::CommandContext commandContext() const override { return qt::CommandContext::Graph; }
    QStringList channelNames() const override;
    std::optional<QString> activeChannel() const override;
    bool acceptsChannelDrop() const override { return true; }
    void addChannel(const QString& name, bool newPanel) override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    ExportJob exportJob(const ExportOptions& options) const override;
    QImage renderToImage(qreal dpr) const override;
    plot::HitResult hitAt(QPointF logical) const override;
    bool cancelTransient() override; // ESC: drops a rubber band in progress
    bool handleCommand(const QString& id, const QVariantList& args) override;

    // Manual vs Channel/Auto scale of group `group` (04 section 8.3 items 1-2): emits specEditRequested.
    // `unit` is the unit the manual limits are given in ("" = base).
    void setGroupScale(int group, plot::ScaleMode mode, std::optional<double> min = std::nullopt,
                       std::optional<double> max = std::nullopt, const QString& unit = {});
    // Makes `name` the active channel (key label click): emits specEditRequested.
    void setActiveChannel(const QString& name);
    // M4 (D-211 (e)): moves group `from` to index `to` of the groups (one undoable spec edit; false when
    // either is out of range or they are equal) and opens the Properties dialog at group `group`'s record
    // (Group Properties; nullptr when there is no such group).
    bool moveGroup(int from, int to);
    PropertiesDialog* openGroupProperties(int group);
    // The insertion index and the line (device px y) a group drag shows now (nullopt when none).
    std::optional<int> groupDropIndex() const noexcept { return groupDropIndex_; }

    // ---- the graph's commands (body edits are undoable spec edits)
    void toggleVariance();     // F3
    void toggleMeasurements(); // M
    void toggleKey();          // L: the key column (keyMode none <-> labels)
    void toggleStatusStrip();  // E
    void toggleDeviceEvents(); // SHIFT+E
    void cycleCursorStyle();   // Y: line -> crosshair -> none
    void cycleLayout();        // G: panels <-> overlapped
    void cycleStyle();         // S: line <-> points
    // CTRL+SHIFT+C: "name<TAB>value<TAB>unit" per configured channel at the Main cursor (Main slot); with a
    // datum "name<TAB>value@main<TAB>value@datum<TAB>delta<TAB>unit". copyCursorValues puts it on the
    // clipboard.
    QString cursorValuesText() const;
    void copyCursorValues();
    // Vertical zoom and pan of the active group (04 section 8.3 item 6; local view state).
    void verticalZoom(double factor); // < 1 zooms in about the active channel's cursor value, else the centre
    void verticalZoomFull(int group = -1);   // -1: the active group
    void verticalPan(double fractionOfSpan); // + up
    void setVerticalZoom(int group, plot::TdGroupZoom zoom);
    plot::TdGroupZoom verticalZoomOf(int group) const;
    // The offset step of a visible slot (04 section 3.3 item 3): one sample period in microseconds of the
    // active channel in that slot's source (time), or analysis::DistanceIndex::kStepM (distance); nullopt
    // without one.
    std::optional<double> offsetStep(int slot) const;
    // Annotations of the Main source (04 section 8.11 item 4): undoable sidecar edits through the selection's
    // SidecarCommit hook; false without one.
    bool addAnnotationAtCursor(const QString& text);
    bool editAnnotation(const model::MarkerId& id, const QString& text);
    bool removeAnnotation(const model::MarkerId& id);

    const plot::TimeDistanceBody& body() const noexcept { return body_; }
    const qt::LayerCache& layers() const noexcept { return layers_; }
    // The hit regions of the last delivered Static build (device px).
    const plot::HitTester& hits() const noexcept { return hits_; }
    // What the last delivered Static build placed (bands, markers, measurements, scales, scroll extent).
    const plot::TdStaticInfo& staticInfo() const noexcept { return info_; }
    const plot::TdLocalState& localState() const noexcept { return local_; }
    // The inputs of the current layers and their layout (device px); empty until the first build request.
    const plot::ComponentBuildInputs& inputs() const noexcept { return inputs_; }
    const plot::TdLayout& layout() const noexcept { return layout_; }
    // The view version of the last delivered Static build.
    std::uint64_t builtViewVersion() const noexcept { return builtViewVersion_; }
    // No Static build is dirty, scheduled or in flight: the Static image shows the current inputs.
    bool staticUpToDate() const noexcept;
    bool pendingStripeShown() const noexcept { return pendingShown_; }
    // Counters (tests, bench): Static builds submitted and delivered, Dynamic and Chrome lists built. A build
    // replaced while still queued is never delivered (01 section 5.4 rule 2), so delivered may stay below
    // submitted for good; staticBuildShown() is the number (1-based, in submission order) of the build whose
    // image is shown, staticBuildsSubmitted() once the latest one is delivered.
    int staticBuildsSubmitted() const noexcept { return staticSubmitted_; }
    int staticBuildsDelivered() const noexcept { return staticDelivered_; }
    int staticBuildShown() const noexcept { return staticShown_; }
    int dynamicBuilds() const noexcept { return dynamicBuilds_; }
    int chromeBuilds() const noexcept { return chromeBuilds_; }
    // The damage of the last Dynamic rebuild (logical px) and the region of the last paintEvent.
    QRegion lastDynamicDamage() const { return lastDamage_; }
    QRegion lastPaintRegion() const { return lastPaint_; }
    bool rubberBandActive() const noexcept { return bandStart_.has_value(); }
    // The measurement hover line (device px x), when one is drawn.
    std::optional<float> hoverLineX() const noexcept { return hoverLineX_; }
    // The text a tooltip at `logical` would show ("" none).
    QString toolTipAt(QPointF logical) const;

    // Domain value (us or m from the Main range start) at a widget-logical x, clamped to the trace area;
    // nullopt before the first layout.
    std::optional<double> domainXAt(double logicalX) const;
    // Widget-logical x of a domain value (may lie outside the trace area).
    double logicalXOf(double domainX) const;
    // Widget-logical x of the cursor, or nullopt when it is not drawn (distance mode without distance).
    std::optional<double> cursorLogicalX() const;

    // Tests and the bench: sees every delivered Static build before it is rasterised.
    using BuildObserver = std::function<void(const plot::ComponentBuild& build)>;
    void setBuildObserver(BuildObserver observer) { observer_ = std::move(observer); }

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void onFocusChanged(bool focused) override;
    int captionHeight() const override;
    // M4 (D-211 (f)): the Main slot's mapping of the prepared inputs (an unlinked part's own view).
    std::optional<plot::DomainMapper> mainMapper() const override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // The active channel's value at the cursor as a fraction of group `group`'s shown scale (ALT+UP/DOWN).
    std::optional<double> cursorValueFraction(int group) const;
    void releaseSources();
    enum class Drag : std::uint8_t { None, Cursor, Pan, Thumb, VPan, VThumb, Offset, Handle, Group };
    enum class BandAxis : std::uint8_t { Horizontal, Vertical, Box };

    QString componentId() const;
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
    void scheduleStaticBuild();
    void requestStaticBuild();
    void prepare();
    void applyOverrides(plot::ComponentBuildInputs& in) const;
    void onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result, QImage raster,
                       std::shared_ptr<plot::TdStaticInfo> info);
    void rebuildDynamic();
    void rebuildChrome();
    void setPendingShown(bool shown);
    void updateWorksheetHints();
    void setCursorAtLogicalX(double logicalX);
    void setCursorAtDomainX(double x, bool snap);
    void startRubberBand(QPointF logical, BandAxis axis);
    void endRubberBand();
    void finishRubberBand(QPointF logical);
    const plot::ResolvedChannel* activeMainChannel() const;
    const plot::TdGroupLayout* groupLayout(int group) const;
    int activeGroup() const;
    bool showsSource(const model::SourceId& source) const;
    std::optional<units::QuantityId> quantityOf(const std::string& channel) const;
    void requestBody(const plot::TimeDistanceBody& body, const QString& undoText);
    void changeVerticalZoom(int group, plot::TdGroupZoom zoom);
    double dprOrOne() const;
    void showAnnotationMenu(const plot::HitResult& hit, QPoint globalPos);
    const plot::TdMarkerEntry* markerOf(const model::MarkerId& id) const;

    plot::TimeDistanceBody body_;
    std::shared_ptr<const project::Scheme> scheme_;
    std::shared_ptr<const qt::SchemeFonts> fonts_;
    std::shared_ptr<const plot::ITextMetrics> metrics_;
    std::uint64_t schemeGeneration_ = 1;
    qt::LayerCache layers_;
    plot::ComponentBuildInputs inputs_;
    plot::TdLayout layout_;
    plot::HitTester hits_;
    plot::TdStaticInfo info_;
    plot::TdLocalState local_;
    float hitsDpr_ = 1.0F; // the DPR of the Static list the hit regions came with
    bool prepared_ = false;
    bool staticDirty_ = true;
    bool dynamicDirty_ = false; // a view change while hidden: the Dynamic layer is rebuilt when shown
    bool buildScheduled_ = false;
    bool buildInFlight_ = false;
    bool pendingShown_ = false;
    std::uint64_t submitSerial_ = 0;
    std::uint64_t builtViewVersion_ = 0;
    int staticSubmitted_ = 0;
    int staticDelivered_ = 0;
    int staticShown_ = 0;
    int dynamicBuilds_ = 0;
    int chromeBuilds_ = 0;
    QRegion lastDamage_;
    QRegion lastPaint_;
    QTimer* pendingTimer_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QTimer* releaseTimer_ = nullptr;
    Drag drag_ = Drag::None;
    double dragStartX_ = 0.0;
    double dragStartY_ = 0.0;
    plot::XSpan dragStartZoom_;
    plot::TdGroupZoom dragStartVZoom_;
    plot::HitResult dragHit_;           // the region a drag started on (handles, offset axis, scrollbars)
    double dragLastX_ = 0.0;            // offset drag: the last pointer x (logical)
    std::optional<float> guideY_;       // Lo/Hi or splitter drag: the guide line (device px)
    std::optional<int> groupDropIndex_; // M4: a group heading drag: where the group goes
    std::optional<float> groupDropY_;   // and its line (device px)
    void updateGroupDrop(double logicalY);
    std::optional<double> bandStart_; // rubber band, domain units
    double bandEnd_ = 0.0;
    BandAxis bandAxis_ = BandAxis::Horizontal;
    float bandStartY_ = 0.0F; // device px
    float bandEndY_ = 0.0F;
    int bandGroup_ = -1;
    std::optional<float> hoverLineX_;
    int wheelRemainder_ = 0; // angleDelta units not yet a whole notch (120)
    BuildObserver observer_;
};

} // namespace esses::widgets
