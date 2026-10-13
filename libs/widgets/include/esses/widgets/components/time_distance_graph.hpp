// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceGraph, JSON type "time_distance" (04 section 8; AC-M1-01, -02, -04, -05, -09). M1 scope (07
// section 4.1): groups and panels, like-units grouping, Manual vs Channel/Auto scale, cursor, zoom, pan, F9,
// overlays in both domains, D-017 colours; overlapped mode, measurements, variance, bands, the scrollbar and
// the status strip later. The layers come from plot::TimeDistanceBuilder (the same builders as
// OffscreenRenderer, so a Print to Clipboard equals esses-render), painted by a qt::LayerCache:
//   Static   a worker build through RenderService::submitBuild (replace key trace/<worksheet>/<component>,
//            Priority::Visible; TraceBuilder::decimateMany and the trace emission fan out on the TaskPool),
//            rasterised by the same worker (RasterBackend::rasteriseStatic, parallel strips; 04 section 6.3
//            step 2 "worker build + raster") and delivered on the GUI thread through the Dispatcher, which
//            only stores the image (LayerCache::storeStatic; LayerCache::rasteriseStatic when the worker had
//            no image, e.g. a cancelled raster). Every request
//            prepares its inputs with RenderService::prepareInputs (the current ViewState, SelectionSnapshot,
//            device size and DPR). Requests are coalesced to one per event-loop turn; a delivered build is
//            always shown (a superseded one as an interim frame, 04 section 6.3 step 3).
//   Dynamic  built synchronously on the GUI thread on Cursor/Datum (and drag feedback), painted under
//            LayerCache::dynamicDamage (old and new cursor strips plus the key values): a cursor move never
//            submits a Static build and never repaints the rest of the component (04 section 6.2, AC-M1-02).
//            Key values still waiting for a chunk ("...") are retried every 50 ms.
//   Chrome   caption, focus border (the worksheet's focused component), active-channel square, the pending
//            stripe and the "no distance data" banner; rebuilt on focus, spec, scheme and pending changes.
// Mouse (04 sections 8.5-8.6), routed through hitAt (the HitTester delivered with the Static layer): a click
// or drag in a panel places the cursor, snapped to the nearest sample of the active channel in the Main slot
// (WorksheetViewModel::setCursor, CursorSource::Click); the wheel zooms x0.8 / x1.25 about the pointer;
// dragging the x axis pans continuously; a double-click in a panel starts a rubber band (Dynamic layer) that
// the next click turns into a zoom (ESC cancels); a click on a key label makes that channel active; a
// double-click on the caption maximises (WorksheetCanvas). Keyboard zoom/pan/cursor and F9 are the qtmodel
// commands acting on the WorksheetViewModel (registerViewCommands); this widget only follows the view
// (commandContext() Graph).
// While it is the worksheet's focused component it reports its trace width (WorksheetViewModel::setSize,
// cursor steps) and its active channel's sample period (setActiveSamplePeriod, zoom minimum).
// Like-units grouping (04 section 8.2 item 3): addChannel joins the first group whose quantity (that of its
// first channel resolvable in the Main source) equals the channel's; newPanel (CTRL) or no match appends a
// new group named after the channel. Edits go out as specEditRequested; applySpec rebuilds.
// Memory (04 section 6.2): a component hidden for 30 s releases its Static image; showing it rebuilds.
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

class TimeDistanceGraph final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "time_distance";
    static constexpr int kPendingStripeDelayMs = 100; // the stripe shows only for a build that takes longer
    static constexpr int kValueRetryMs = 50;          // "..." key values waiting for a chunk
    static constexpr int kReleaseHiddenMs = 30'000;   // 04 section 6.2 memory rule (D-058)

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
    QImage renderToImage(qreal dpr) const override;
    plot::HitResult hitAt(QPointF logical) const override;
    bool cancelTransient() override; // ESC: drops a rubber band in progress

    // Manual vs Channel/Auto scale of group `group` (04 section 8.3 items 1-2; the Group Properties dialog
    // arrives in M3): emits specEditRequested. `unit` is the unit the manual limits are given in ("" = base).
    void setGroupScale(int group, plot::ScaleMode mode, std::optional<double> min = std::nullopt,
                       std::optional<double> max = std::nullopt, const QString& unit = {});
    // Makes `name` the active channel (key label click): emits specEditRequested.
    void setActiveChannel(const QString& name);

    const plot::TimeDistanceBody& body() const noexcept { return body_; }
    const qt::LayerCache& layers() const noexcept { return layers_; }
    // The hit regions of the last delivered Static build (device px).
    const plot::HitTester& hits() const noexcept { return hits_; }
    // The inputs of the current layers and their layout (device px); empty until the first build request.
    const plot::ComponentBuildInputs& inputs() const noexcept { return inputs_; }
    const plot::TdLayout& layout() const noexcept { return layout_; }
    // The view version of the last delivered Static build.
    std::uint64_t builtViewVersion() const noexcept { return builtViewVersion_; }
    // No Static build is dirty, scheduled or in flight: the Static image shows the current inputs.
    bool staticUpToDate() const noexcept;
    bool pendingStripeShown() const noexcept { return pendingShown_; }
    // Counters (tests, bench): Static builds submitted and delivered, Dynamic and Chrome lists built.
    int staticBuildsSubmitted() const noexcept { return staticSubmitted_; }
    int staticBuildsDelivered() const noexcept { return staticDelivered_; }
    int dynamicBuilds() const noexcept { return dynamicBuilds_; }
    int chromeBuilds() const noexcept { return chromeBuilds_; }
    // The damage of the last Dynamic rebuild (logical px) and the region of the last paintEvent.
    QRegion lastDynamicDamage() const { return lastDamage_; }
    QRegion lastPaintRegion() const { return lastPaint_; }
    bool rubberBandActive() const noexcept { return bandStart_.has_value(); }

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
    void onFocusChanged(bool focused) override;
    int captionHeight() const override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class Drag : std::uint8_t { None, Cursor, Pan };

    QString componentId() const;
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
    void scheduleStaticBuild();
    void requestStaticBuild();
    void prepare();
    void onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result, QImage raster);
    void rebuildDynamic();
    void rebuildChrome();
    void setPendingShown(bool shown);
    void updateWorksheetHints();
    void setCursorAtLogicalX(double logicalX);
    void startRubberBand(double domainX);
    void endRubberBand();
    void finishRubberBand(double logicalX);
    const plot::ResolvedChannel* activeMainChannel() const;
    bool showsSource(const model::SourceId& source) const;
    std::optional<units::QuantityId> quantityOf(const std::string& channel) const;
    void requestBody(const plot::TimeDistanceBody& body, const QString& undoText);

    plot::TimeDistanceBody body_;
    std::shared_ptr<const project::Scheme> scheme_;
    std::shared_ptr<const qt::SchemeFonts> fonts_;
    std::shared_ptr<const plot::ITextMetrics> metrics_;
    std::uint64_t schemeGeneration_ = 1;
    qt::LayerCache layers_;
    plot::ComponentBuildInputs inputs_;
    plot::TdLayout layout_;
    plot::HitTester hits_;
    float hitsDpr_ = 1.0F; // the DPR of the Static list the hit regions came with
    bool prepared_ = false;
    bool staticDirty_ = true;
    bool buildScheduled_ = false;
    bool buildInFlight_ = false;
    bool pendingShown_ = false;
    std::uint64_t submitSerial_ = 0;
    std::uint64_t builtViewVersion_ = 0;
    int staticSubmitted_ = 0;
    int staticDelivered_ = 0;
    int dynamicBuilds_ = 0;
    int chromeBuilds_ = 0;
    QRegion lastDamage_;
    QRegion lastPaint_;
    QTimer* pendingTimer_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QTimer* releaseTimer_ = nullptr;
    Drag drag_ = Drag::None;
    double dragStartX_ = 0.0;
    plot::XSpan dragStartZoom_;
    std::optional<double> bandStart_; // rubber band, domain units
    double bandEnd_ = 0.0;
    int wheelRemainder_ = 0; // angleDelta units not yet a whole notch (120)
    BuildObserver observer_;
};

} // namespace esses::widgets
