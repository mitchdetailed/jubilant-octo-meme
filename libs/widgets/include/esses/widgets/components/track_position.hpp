// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackPosition, JSON type "track_position" (04 section 14; D-027; 07 section 6.1).
// The venue's TrackMap (ComponentBuildInputs::trackMap: RenderService::prepareInputs binds the Main source's
// venue through TrackMapService) drawn by plot::TrackGeometry, the same builders as OffscreenRenderer:
//   Static   a worker build through RenderService::submitBuild (replace key trace/<worksheet>/<component>,
//            Visible), rasterised by the worker, stored by a qt::LayerCache on the GUI thread; on Data,
//            Selection, Scheme, Size and a map change (TrackMapService::mapChanged) and on the component's
//            own zoom/pan/fit
//   Dynamic  the zoom span and a cursor dot per visible slot, built on the GUI thread on Cursor, Datum, Zoom
//            and Domain (04 section 7.4: never a Static build for those)
//   Chrome   caption, focus border, the "no track map (Tools -> Track Editor)" and "no distance data" banners
// Interaction (04 section 14 item 4, 06 section 9): PLUS ("+", "=") and MINUS zoom about the centre
// (track.zoom_in / track.zoom_out, key context Track), the wheel about the pointer (zoom.wheel), a drag pans
// and a double-click fits (track.fit), each while the component is the worksheet's focused component or has
// the keyboard focus (the Track Map dock). They set the body's scale {mode manual, zoom, panX, panY} at once
// and commit it as one spec edit when the gesture rests (kCommitDelayMs; "Zoom track view"); fit returns to
// mode "fit". The shell offers the track.* rows to the focused component (handleCommand); where no command
// runs (the Track Map dock, or no shell) the keys reach keyPressEvent. A backgrounded map ("background":
// true) does not consume clicks (they reach the canvas). A component hidden for 30 s releases its Static
// image (D-058). Overlay policy AllOverlays (04 section 7.4); key context Track (06 section 14.2).
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/track_geometry.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QPointF>
#include <QPointer>
#include <cstdint>
#include <memory>
#include <optional>

class QTimer;

namespace esses::widgets {

class TrackPosition final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "track_position";
    static constexpr double kZoomStep = 1.25; // one PLUS/MINUS press or wheel notch
    static constexpr double kMinZoom = 0.2;   // scale.zoom limits
    static constexpr double kMaxZoom = 64.0;
    static constexpr int kCommitDelayMs = 400;      // zoom/pan gestures coalesce into one spec edit
    static constexpr int kReleaseHiddenMs = 30'000; // 04 section 6.2 memory rule (D-058)

    TrackPosition(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~TrackPosition() override;

    const char* type() const override { return kType; }
    void applySpec(const project::ComponentSpec& spec) override;
    void onViewChanged(const plot::ViewState& view, plot::ChangeMask what) override;
    void onSelectionChanged(const plot::SelectionSnapshot& selection) override;
    void onDataChanged(const model::SourceId& source, std::uint64_t version) override;
    void onChannelDataChanged(const maths::ChannelKey& key) override; // draws no channel: ignored
    void onSchemeChanged(const project::Scheme& scheme) override;
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    QStringList contextCommands() const override;
    QImage renderToImage(qreal dpr) const override;
    plot::HitResult hitAt(QPointF logical) const override;
    // track.zoom_in, track.zoom_out and track.fit (the shell offers them to the focused component).
    bool handleCommand(const QString& id, const QVariantList& args) override;

    // The view gestures (also the track.* commands). `about`: a widget-logical point (the wheel), else the
    // map area's centre.
    void zoomBy(double factor, std::optional<QPointF> about = std::nullopt);
    void panByLogical(QPointF delta);
    void fit();
    // Commits a pending zoom/pan edit now (else kCommitDelayMs after the last step).
    void commitView();

    const plot::TrackPositionBody& body() const noexcept { return body_; }
    // The map of the last prepared inputs (null: none bound).
    std::shared_ptr<const analysis::TrackMap> trackMap() const { return inputs_.trackMap; }
    // Tests: the prepared inputs, the builds, whether the Static layer shows the current state.
    const plot::ComponentBuildInputs& inputs() const noexcept { return inputs_; }
    int staticSubmitted() const noexcept { return staticSubmitted_; }
    int staticDelivered() const noexcept { return staticDelivered_; }
    int dynamicBuilds() const noexcept { return dynamicBuilds_; }
    bool staticUpToDate() const noexcept;
    const qt::LayerCache& layers() const noexcept { return layers_; }

protected:
    void onFocusChanged(bool focused) override;
    int captionHeight() const override;
    void paintEvent(QPaintEvent* event) override;
    // M4 (D-211 (f)): the Main slot's mapping of the prepared inputs (an unlinked part's own view).
    std::optional<plot::DomainMapper> mainMapper() const override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void releaseInputs();
    QString componentId() const;
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
    void scheduleStaticBuild();
    void requestStaticBuild();
    void prepare();
    void rebuildDynamic();
    void rebuildChrome();
    bool interactive() const;
    bool inMapArea(QPointF logical) const;
    // The device-px map area (below the caption, inset as plot::TrackGeometry draws it) and its fit.
    plot::RectPx mapAreaPx() const;
    void setLocalBody(const plot::TrackPositionBody& body);

    plot::TrackPositionBody body_;
    std::shared_ptr<const project::Scheme> scheme_;
    std::shared_ptr<const qt::SchemeFonts> fonts_;
    std::shared_ptr<const plot::ITextMetrics> metrics_;
    std::uint64_t schemeGeneration_ = 0;
    qt::LayerCache layers_;
    plot::ComponentBuildInputs inputs_;
    plot::HitTester hits_;
    float hitsDpr_ = 1.0F;
    QPointer<QObject> mapsConnected_; // the TrackMapService whose mapChanged rebuilds this map
    QTimer* commitTimer_ = nullptr;
    QTimer* releaseTimer_ = nullptr;
    std::optional<QPointF> dragFrom_;
    bool prepared_ = false;
    bool staticDirty_ = true;
    bool dynamicDirty_ = false; // a view change while hidden: the Dynamic layer is rebuilt when shown
    bool buildScheduled_ = false;
    bool buildInFlight_ = false;
    std::uint64_t submitSerial_ = 0;
    int staticSubmitted_ = 0;
    int staticDelivered_ = 0;
    int dynamicBuilds_ = 0;
};

} // namespace esses::widgets
