// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayeredComponent (04 sections 6.2-6.3 and 7; D-198): the layer pipeline of the M3 drawn components that
// render through a plotcore geometry builder (OutingGraph, ScatterPlot, Histogram, NumericGauge, BarGauge),
// the pipeline TimeDistanceGraph has for its own builder:
//   Static   a worker build through RenderService::submitBuild (replace key trace/<worksheet>/<component>,
//            Priority::Visible), rasterised by the same worker (RasterBackend::rasteriseStatic) and delivered
//            on the GUI thread, which only stores the image; inputs from RenderService::prepareInputs (view,
//            selection, device size, DPR), adjusted by the subclass (adjustInputs: a scatter's own axis
//            zoom); one request per event-loop turn; a delivered build is always shown (a superseded one as
//            an interim frame). The change bits that rebuild it are the subclass's staticMask (04
//            section 7.4: e.g. Zoom rebuilds a scatter's points but only the outing graph's Dynamic zoom
//            rectangle).
//   Dynamic  built synchronously on the GUI thread for every other change bit (the cursor, the datum; the
//            outing's zoom), painted under LayerCache::dynamicDamage; values still waiting for a chunk
//            ("...") are retried every 50 ms.
//   Chrome   caption, focus border, pending stripe; rebuilt on focus, spec, scheme and pending changes.
// A component hidden for 30 s releases its Static image (D-058); showing it rebuilds. Mouse input is routed
// through hitAt (the HitTester delivered with the Static layer). channelNames() lists
// plot::componentChannelUses. Spec edits go out as specEditRequested (requestSpec); applySpec rebuilds.
// renderToImage is the current composite at the current DPR (Print to Clipboard, 04 section 7.2 item 9), at
// another DPR the Static image scaled with Dynamic and Chrome rebuilt. GUI thread only.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QRegion>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace esses::widgets {

class LayeredComponent : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr int kPendingStripeDelayMs = 100; // the stripe shows only for a build that takes longer
    static constexpr int kValueRetryMs = 50;          // "..." values waiting for a chunk
    static constexpr int kReleaseHiddenMs = 30'000;   // 04 section 6.2 memory rule (D-058)

    // The three builders of the component's type (the same functions as OffscreenRenderer's, AC-M1-05).
    struct Builders {
        std::function<Result<plot::ComponentBuild>(const plot::ComponentBuildInputs&, const CancelToken&)>
            buildStatic;
        std::function<plot::DrawList(const plot::ComponentBuildInputs&)> buildDynamic;
        std::function<plot::DrawList(const plot::ComponentBuildInputs&, bool pending)> buildChrome;
    };

    ~LayeredComponent() override;

    void applySpec(const project::ComponentSpec& spec) override;
    void onViewChanged(const plot::ViewState& view, plot::ChangeMask what) override;
    void onSelectionChanged(const plot::SelectionSnapshot& selection) override;
    void onDataChanged(const model::SourceId& source, std::uint64_t version) override;
    void onChannelDataChanged(const maths::ChannelKey& key) override; // only the channels it draws
    void onSchemeChanged(const project::Scheme& scheme) override;
    QStringList channelNames() const override; // plot::componentChannelUses
    QImage renderToImage(qreal dpr) const override;
    plot::HitResult hitAt(QPointF logical) const override;

    const qt::LayerCache& layers() const noexcept { return layers_; }
    // The hit regions of the last delivered Static build (device px).
    const plot::HitTester& hits() const noexcept { return hits_; }
    // The inputs of the current layers (adjusted); empty until the first build request.
    const plot::ComponentBuildInputs& inputs() const noexcept { return inputs_; }
    // No Static build is dirty, scheduled or in flight: the Static image shows the current inputs.
    bool staticUpToDate() const noexcept;
    bool pendingStripeShown() const noexcept { return pendingShown_; }
    std::uint64_t builtViewVersion() const noexcept { return builtViewVersion_; }
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
    // The change bits that rebuild the Static layer (the rest rebuild the Dynamic layer only).
    plot::ChangeMask staticMask() const noexcept { return staticMask_; }

    // Tests and the bench: sees every delivered Static build before it is rasterised.
    using BuildObserver = std::function<void(const plot::ComponentBuild& build)>;
    void setBuildObserver(BuildObserver observer) { observer_ = std::move(observer); }

protected:
    LayeredComponent(const project::ComponentSpec& spec, WorksheetContext& context, Builders builders,
                     plot::ChangeMask staticMask, QWidget* parent);

    // Called on the prepared inputs before every build (Static, Dynamic, Chrome): a subclass's transient view
    // state that is not persisted (the scatter's own axis zoom).
    virtual void adjustInputs(plot::ComponentBuildInputs& inputs) const;
    // Called on the GUI thread with every delivered Static build, before its image is shown.
    virtual void onStaticDelivered(const plot::ComponentBuild& build);
    // Extra Dynamic content drawn by the widget (a rubber band); appended to the builder's Dynamic list.
    virtual void appendDynamic(plot::DrawList& list) const;
    // The spec changed (applySpec); subclasses re-parse their body here before the rebuild.
    virtual void onSpecApplied();
    // The sources whose data the component shows; every visible slot by default.
    virtual bool showsSource(const model::SourceId& source) const;

    // Emits specEditRequested with `next` (the canvas wraps it in an undo command and calls applySpec).
    void requestSpec(const project::ComponentSpec& next, const QString& undoText);
    // The JSON body replaced, the rest of the spec kept.
    void requestBody(const JsonDoc& body, const QString& undoText);
    void scheduleStaticBuild(); // marks the Static layer dirty and requests a build this event-loop turn
    void rebuildDynamic();
    void rebuildChrome();
    bool prepared() const noexcept { return prepared_; }
    // Bumped whenever the inputs are prepared again or released: geometry derived from inputs() alone may be
    // cached under it (D-203 (ab)).
    std::uint64_t inputsRevision() const noexcept { return inputsRevision_; }
    // Device px of a widget-logical point, in the DPR of the hit regions.
    QPointF devicePoint(QPointF logical) const;
    float hitsDpr() const noexcept { return hitsDpr_ > 0.0F ? hitsDpr_ : 1.0F; }
    // A copy of the current inputs with the live view (the Dynamic layer's view).
    plot::ComponentBuildInputs liveInputs() const;
    QString componentId() const;
    const Builders& builders() const noexcept { return builders_; }
    std::shared_ptr<const project::Scheme> scheme() const { return scheme_; }

    void onFocusChanged(bool focused) override;
    int captionHeight() const override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void releaseInputs();
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
    void requestStaticBuild();
    void prepare();
    void onStaticBuilt(std::uint64_t serial, Result<plot::ComponentBuild> result, QImage raster);
    void setPendingShown(bool shown);

    Builders builders_;
    plot::ChangeMask staticMask_ = 0;
    std::shared_ptr<const project::Scheme> scheme_;
    std::shared_ptr<const qt::SchemeFonts> fonts_;
    std::shared_ptr<const plot::ITextMetrics> metrics_;
    std::uint64_t schemeGeneration_ = 1;
    qt::LayerCache layers_;
    plot::ComponentBuildInputs inputs_;
    plot::HitTester hits_;
    float hitsDpr_ = 1.0F;
    bool prepared_ = false;
    bool staticDirty_ = true;
    bool dynamicDirty_ = false; // a view change while hidden: the Dynamic layer is rebuilt when shown
    bool buildScheduled_ = false;
    bool buildInFlight_ = false;
    bool pendingShown_ = false;
    std::uint64_t submitSerial_ = 0;
    std::uint64_t inputsRevision_ = 1;
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
    BuildObserver observer_;
};

} // namespace esses::widgets
