// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ZoomSummaryBar (06 sections 2.1 and 4.5, 04 section 8.6). Owner: src/properties_shell/.

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/raster_backend.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace esses::widgets {

namespace {

constexpr int kMarginPx = 4;              // logical
constexpr int kBarInsetPx = 3;            // logical, top and bottom of the outing bar
constexpr int kHeightPx = 18;             // logical
constexpr double kNoDistanceLength = 1.0; // extent'(R) without an index (04 section 3.4)

plot::Rgba withAlpha(plot::Rgba c, std::uint32_t alpha) {
    return (c & 0xFFFFFF00U) | (alpha & 0xFFU);
}

const model::Range* outingContaining(const model::DataLayer& layer, Micros t) {
    const model::RangeGroup* outings = layer.ranges.find("Outings");
    if (outings == nullptr) {
        return nullptr;
    }
    const model::Range* best = nullptr;
    for (const model::Range& r : outings->ranges) {
        if (t >= r.start && t < r.end) {
            return &r;
        }
        if (best == nullptr || std::abs(r.start - t) < std::abs(best->start - t)) {
            best = &r;
        }
    }
    return best;
}

} // namespace

ZoomSummaryBar::ZoomSummaryBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("zoomSummaryBar"));
    setMinimumHeight(kHeightPx);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setToolTip(
        tr("Zoom summary: click sets the cursor, drag moves the zoom, Shift+click extends it, double-click "
           "shows the whole outing"));
}

ZoomSummaryBar::~ZoomSummaryBar() = default;

void ZoomSummaryBar::setWorksheet(qt::WorksheetViewModel* worksheet) {
    if (worksheet_ == worksheet) {
        return;
    }
    if (worksheet_ != nullptr) {
        QObject::disconnect(worksheet_, nullptr, this, nullptr);
    }
    worksheet_ = worksheet;
    if (worksheet_ != nullptr) {
        connect(worksheet_, &qt::WorksheetViewModel::changed, this, [this](plot::ChangeMask what) {
            constexpr plot::ChangeMask relevant =
                plot::Change::Zoom | plot::Change::Cursor | plot::Change::Datum | plot::Change::Domain |
                plot::Change::Selection | plot::Change::Data | plot::Change::Offsets;
            if ((what & relevant) != 0) {
                recompute();
            }
        });
        connect(worksheet_, &QObject::destroyed, this, [this] {
            worksheet_ = nullptr;
            recompute();
        });
    }
    recompute();
}

void ZoomSummaryBar::setSelection(qt::DataSelectionModel* selection) {
    if (selection_ == selection) {
        return;
    }
    if (selection_ != nullptr) {
        QObject::disconnect(selection_, nullptr, this, nullptr);
    }
    selection_ = selection;
    if (selection_ != nullptr) {
        connect(selection_, &qt::DataSelectionModel::sourcesChanged, this, [this] { recompute(); });
    }
    recompute();
}

void ZoomSummaryBar::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    scheme_ = std::move(scheme);
    update();
}

QSize ZoomSummaryBar::sizeHint() const {
    return {200, kHeightPx};
}

double ZoomSummaryBar::xOfT(double tRel) const {
    if (domain_ == plot::Domain::Time) {
        return tRel;
    }
    const auto abs = static_cast<Micros>(std::llround(static_cast<double>(geom_.start) + tRel));
    if (chain_ != nullptr && chain_->indexedAt(abs)) {
        return chain_->distAt(abs);
    }
    const auto len = static_cast<double>(geom_.end - geom_.start);
    return len > 0.0 ? tRel / len * plot::mainExtentIn(domain_, geom_, chain_.get(), kNoDistanceLength) : 0.0;
}

std::optional<double> ZoomSummaryBar::timeOfX(double x) const {
    if (!summary_.valid) {
        return std::nullopt;
    }
    if (domain_ == plot::Domain::Time) {
        return x;
    }
    if (chain_ != nullptr && chain_->anyIndexed()) {
        const plot::XSpan ext = chain_->extent();
        if (x >= ext.a && x <= ext.b) {
            const Micros t = chain_->timeAt(x);
            if (chain_->indexedAt(t)) {
                return static_cast<double>(t - geom_.start);
            }
        }
    }
    const double extent = plot::mainExtentIn(domain_, geom_, chain_.get(), kNoDistanceLength);
    return extent > 0.0 ? x / extent * static_cast<double>(geom_.end - geom_.start) : 0.0;
}

void ZoomSummaryBar::recompute() {
    summary_ = Summary{};
    chain_.reset();
    if (worksheet_ == nullptr) {
        update();
        return;
    }
    qt::DataSelectionModel& sel = selection_ != nullptr ? *selection_ : worksheet_->selectionModel();
    const plot::ViewState& vs = worksheet_->state();
    domain_ = vs.domain;
    summary_.zoom = vs.zoom;
    const model::RangeRef main = sel.snapshot().main;
    const model::Range* r = sel.hasMain() ? sel.resolve(main) : nullptr;
    const auto src = r == nullptr ? nullptr : sel.source(main.src);
    const model::DataLayer* layer = src == nullptr ? nullptr : src->layer(main.layer);
    if (r == nullptr || layer == nullptr || r->end <= r->start) {
        update();
        return;
    }
    geom_ = plot::RangeGeom{r->start, r->end, 0, 0.0, r->stretch.value_or(1.0)};
    plot::OutingGeom outing{r->start, r->end};
    if (r->kind != model::RangeKind::Outing) {
        if (const model::Range* o = outingContaining(*layer, r->start)) {
            outing = plot::OutingGeom{o->start, o->end};
        } else {
            outing = plot::OutingGeom{0, std::max(layer->duration, r->end)};
        }
    }
    chain_ = worksheet_->distanceService().chain(main, Priority::Visible);
    summary_.valid = true;
    summary_.extent = plot::outingExtentIn(domain_, geom_, outing, chain_.get(), kNoDistanceLength);
    summary_.main = plot::XSpan{0.0, plot::mainExtentIn(domain_, geom_, chain_.get(), kNoDistanceLength)};
    // The lap boundaries are the laps' whatever Main is (a lap, an outing, a track section; D-203 (q)).
    if (const model::RangeGroup* laps = layer->ranges.find("Outings:Laps")) {
        for (const model::Range& lap : laps->ranges) {
            if (lap.start >= outing.start && lap.start < outing.end) {
                summary_.laps.push_back(xOfT(static_cast<double>(lap.start - r->start)));
            }
        }
    }
    summary_.cursor = xOfT(static_cast<double>(vs.cursorT));
    if (vs.datumT) {
        summary_.datum = xOfT(static_cast<double>(*vs.datumT));
    }
    update();
}

double ZoomSummaryBar::pxOf(double x) const {
    const double w = std::max(1, width() - 2 * kMarginPx);
    const double len = summary_.extent.length();
    return kMarginPx + (len > 0.0 ? (x - summary_.extent.a) / len * w : 0.0);
}

std::optional<double> ZoomSummaryBar::xAt(double logicalX) const {
    if (!summary_.valid || !(summary_.extent.length() > 0.0)) {
        return std::nullopt;
    }
    const double w = std::max(1, width() - 2 * kMarginPx);
    const double f = std::clamp((logicalX - kMarginPx) / w, 0.0, 1.0);
    return summary_.extent.a + f * summary_.extent.length();
}

plot::DrawList ZoomSummaryBar::buildDrawList(const Summary& s, const plot::LayoutContext& ctx,
                                             const project::Scheme& scheme) {
    plot::DrawList list;
    list.widthPx = ctx.widthPx;
    list.heightPx = ctx.heightPx;
    list.dpr = ctx.dpr;
    list.clear(scheme.role("cptBackground", 0xF0F0F0FFU));
    const float dpr = ctx.dpr;
    const auto w = static_cast<float>(ctx.widthPx);
    const auto h = static_cast<float>(ctx.heightPx);
    const float m = kMarginPx * dpr;
    const float inset = kBarInsetPx * dpr;
    const plot::RectPx bar{m, inset, std::max(1.0F, w - 2.0F * m), std::max(1.0F, h - 2.0F * inset)};
    list.filledRect(bar, scheme.role("dataBackground", 0xFFFFFFFFU));
    if (s.valid && s.extent.length() > 0.0) {
        const auto px = [&](double x) {
            return bar.x + static_cast<float>((x - s.extent.a) / s.extent.length()) * bar.w;
        };
        const auto clampPx = [&](float x) { return std::clamp(x, bar.x, bar.x + bar.w); };
        // The Main range.
        const float m0 = clampPx(px(s.main.a));
        const float m1 = clampPx(px(s.main.b));
        list.filledRect(plot::RectPx{m0, bar.y, std::max(1.0F, m1 - m0), bar.h},
                        withAlpha(scheme.role("rangeMark", 0x808080FFU), 0x60U));
        // Lap boundaries.
        std::vector<float> ticks;
        for (const double x : s.laps) {
            const float p = std::round(px(x)) + 0.5F;
            if (p >= bar.x && p <= bar.x + bar.w) {
                ticks.insert(ticks.end(), {p, bar.y, p, bar.y + bar.h});
            }
        }
        if (!ticks.empty()) { // logical widths times the DPR (04 section 6.6 item 1)
            list.lines(ticks.data(), ticks.size() / 2, scheme.role("cptForeground2", 0x606060FFU),
                       std::max(1.0F, std::round(dpr)));
        }
        // The zoom span.
        const float z0 = clampPx(px(s.zoom.a));
        const float z1 = clampPx(px(s.zoom.b));
        const plot::RectPx span{z0, bar.y, std::max(1.0F, z1 - z0), bar.h};
        const plot::Rgba zoomColour = scheme.role("cursorZoom", 0x3060C0FFU);
        list.filledRect(span, withAlpha(zoomColour, 0x50U));
        list.rectOutline(span, zoomColour, std::max(1.0F, dpr));
        const auto line = [&](double x, plot::Rgba colour) {
            const float p = std::round(px(x)) + 0.5F;
            if (p >= bar.x && p <= bar.x + bar.w) {
                const float xy[4] = {p, 0.0F, p, h};
                list.lines(xy, 2, colour, std::max(1.0F, dpr));
            }
        };
        if (s.datum) {
            line(*s.datum, scheme.role("cursorDatum", 0x00A000FFU));
        }
        if (s.cursor) {
            line(*s.cursor, scheme.role("cursorMain", 0xFF0000FFU));
        }
    }
    list.rectOutline(bar, scheme.role("cptFrame", 0x808080FFU), std::max(1.0F, std::round(dpr)));
    return list;
}

void ZoomSummaryBar::paintEvent(QPaintEvent*) {
    ++repaints_;
    QPainter p(this);
    const qreal dpr = devicePixelRatioF();
    plot::LayoutContext ctx;
    ctx.dpr = static_cast<float>(dpr);
    ctx.widthPx = static_cast<int>(std::lround(width() * dpr));
    ctx.heightPx = static_cast<int>(std::lround(height() * dpr));
    static const project::Scheme fallback;
    const project::Scheme& scheme = scheme_ != nullptr ? *scheme_ : fallback;
    static const qt::SchemeFonts fonts; // the bar draws no text
    qt::RasterBackend::paintList(p, buildDrawList(summary_, ctx, scheme), fonts);
}

void ZoomSummaryBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || worksheet_ == nullptr) {
        QWidget::mousePressEvent(event);
        return;
    }
    const std::optional<double> x = xAt(event->position().x());
    if (!x) {
        return;
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        worksheet_->extendZoom(*x); // zoom.extend
        dragFrom_.reset();
        return;
    }
    dragFrom_ = x;
    dragZoom_ = worksheet_->state().zoom;
    dragged_ = false;
    event->accept();
}

void ZoomSummaryBar::mouseMoveEvent(QMouseEvent* event) {
    if (!dragFrom_ || worksheet_ == nullptr) {
        return; // dragFrom_ is set between the press and the release of the left button
    }
    const std::optional<double> x = xAt(event->position().x());
    if (!x) {
        return;
    }
    const double d = *x - *dragFrom_;
    if (!dragged_ && std::abs(pxOf(*x) - pxOf(*dragFrom_)) < 3.0) {
        return;
    }
    dragged_ = true;
    worksheet_->zoomTo(plot::XSpan{dragZoom_.a + d, dragZoom_.b + d});
}

void ZoomSummaryBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragFrom_ && !dragged_ && worksheet_ != nullptr) {
        if (const std::optional<double> t = timeOfX(*dragFrom_)) {
            worksheet_->setCursor(static_cast<Micros>(std::llround(*t)), qt::CursorSource::Click);
        }
    }
    dragFrom_.reset();
    dragged_ = false;
}

void ZoomSummaryBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && worksheet_ != nullptr) {
        worksheet_->zoomFull();
        dragFrom_.reset();
        event->accept();
    }
}

} // namespace esses::widgets
