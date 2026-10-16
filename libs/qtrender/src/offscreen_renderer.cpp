// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "paint_internal.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/plotcore/gauge_geometry.hpp>
#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/plotcore/outing_geometry.hpp>
#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/plotcore/track_geometry.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>

#include <QPainter>
#include <QRegion>
#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

constexpr plot::Rgba kWhite = 0xFFFFFFFFU;
constexpr plot::Rgba kFrame = 0x808080FFU;
constexpr plot::Rgba kText = 0x606060FFU;

plot::Rgba role(const plot::ComponentBuildInputs& in, std::string_view name, plot::Rgba fallback) {
    return in.scheme ? in.scheme->role(name, fallback) : fallback;
}

// Background, a 1-logical-px frame and a centred line of text: the placeholder and error Static layer.
plot::DrawList placeholderList(const plot::ComponentBuildInputs& in, const std::string& text) {
    plot::DrawList l;
    l.widthPx = in.layout.widthPx;
    l.heightPx = in.layout.heightPx;
    l.dpr = in.layout.dpr;
    const auto w = static_cast<float>(l.widthPx);
    const auto h = static_cast<float>(l.heightPx);
    const float px = std::max(1.0F, std::round(l.dpr));
    l.clear(role(in, "cptBackground", kWhite));
    const plot::Rgba frame = role(in, "cptFrame", kFrame);
    l.filledRect({0.0F, 0.0F, w, px}, frame);
    l.filledRect({0.0F, h - px, w, px}, frame);
    l.filledRect({0.0F, 0.0F, px, h}, frame);
    l.filledRect({w - px, 0.0F, px, h}, frame);
    if (!text.empty()) {
        l.textRun(text, w / 2.0F, h / 2.0F, plot::TextStyleId::Caption, plot::TextAnchor::Centre,
                  role(in, "cptForeground2", kText), std::max(0.0F, w - 4.0F * px));
    }
    return l;
}

plot::DrawList emptyList(const plot::ComponentBuildInputs& in) {
    plot::DrawList l;
    l.widthPx = in.layout.widthPx;
    l.heightPx = in.layout.heightPx;
    l.dpr = in.layout.dpr;
    return l;
}

QSize deviceSizeOf(QSize logical, qreal dpr) {
    return QSize(static_cast<int>(std::lround(logical.width() * dpr)),
                 static_cast<int>(std::lround(logical.height() * dpr)));
}

} // namespace

struct OffscreenRenderer::Job {
    plot::ComponentBuildInputs inputs;
    ComponentBuilders builders;
    QSize logical;
    qreal dpr = 1.0;
    std::optional<Future<plot::ComponentBuild>> future;
    std::optional<Result<plot::ComponentBuild>> inlineResult;
};

OffscreenRenderer::OffscreenRenderer(std::shared_ptr<const SchemeFonts> fonts, TaskPool* pool)
    : fonts_(fonts ? std::move(fonts) : std::make_shared<const SchemeFonts>()), pool_(pool) {}

ComponentBuilders OffscreenRenderer::placeholderBuilders() {
    ComponentBuilders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in,
                       const CancelToken&) -> Result<plot::ComponentBuild> {
        plot::ComponentBuild build;
        build.list = placeholderList(in, in.spec.type);
        build.viewVersion = in.view.version;
        return build;
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) { return emptyList(in); };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool) { return emptyList(in); };
    return b;
}

namespace {

// The three builders of a plotcore builder class with the TimeDistanceBuilder shape.
template <class Geometry> ComponentBuilders buildersOf() {
    ComponentBuilders b;
    b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
        return Geometry::buildStatic(in, cancel);
    };
    b.buildDynamic = [](const plot::ComponentBuildInputs& in) { return Geometry::buildDynamic(in); };
    b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
        return Geometry::buildChrome(in, pending);
    };
    return b;
}

} // namespace

ComponentBuilders OffscreenRenderer::builtinBuilders(std::string_view type) {
    if (type == "time_distance") {
        return buildersOf<plot::TimeDistanceBuilder>();
    }
    // The M3 drawn components (07 section 6.1): their builders are plotcore's; a Static build that fails
    // renders the "Render failed" placeholder.
    if (type == "outing") {
        return buildersOf<plot::OutingGeometry>();
    }
    if (type == "scatter") {
        return buildersOf<plot::ScatterGeometry>();
    }
    if (type == "histogram") {
        return buildersOf<plot::HistogramGeometry>();
    }
    if (type == "track_position") {
        return buildersOf<plot::TrackGeometry>();
    }
    if (type == "gauge_numeric") {
        ComponentBuilders b;
        b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
            return plot::GaugeGeometry::buildNumericStatic(in, cancel);
        };
        b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
            return plot::GaugeGeometry::buildNumericDynamic(in);
        };
        b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
            return plot::GaugeGeometry::buildNumericChrome(in, pending);
        };
        return b;
    }
    if (type == "gauge_bar") {
        ComponentBuilders b;
        b.buildStatic = [](const plot::ComponentBuildInputs& in, const CancelToken& cancel) {
            return plot::GaugeGeometry::buildBarStatic(in, cancel);
        };
        b.buildDynamic = [](const plot::ComponentBuildInputs& in) {
            return plot::GaugeGeometry::buildBarDynamic(in);
        };
        b.buildChrome = [](const plot::ComponentBuildInputs& in, bool pending) {
            return plot::GaugeGeometry::buildBarChrome(in, pending);
        };
        return b;
    }
    // values_list, channel_report and time_report are table widgets (QWidget::render); unknown types.
    return placeholderBuilders();
}

void OffscreenRenderer::setBuilders(std::string type, ComponentBuilders builders) {
    builders_[std::move(type)] = std::move(builders);
}

ComponentBuilders OffscreenRenderer::buildersFor(const project::ComponentSpec& spec) const {
    if (!spec.known) {
        return placeholderBuilders();
    }
    const auto it = builders_.find(spec.type);
    ComponentBuilders b = it != builders_.end() ? it->second : builtinBuilders(spec.type);
    const ComponentBuilders fallback = placeholderBuilders();
    if (!b.buildStatic) {
        b.buildStatic = fallback.buildStatic;
    }
    if (!b.buildDynamic) {
        b.buildDynamic = fallback.buildDynamic;
    }
    if (!b.buildChrome) {
        b.buildChrome = fallback.buildChrome;
    }
    return b;
}

OffscreenRenderer::Job OffscreenRenderer::start(const plot::ComponentBuildInputs& inputs, QSize logicalPx,
                                                qreal dpr) const {
    Job job;
    job.inputs = inputs;
    job.logical = logicalPx;
    job.dpr = dpr > 0.0 ? dpr : 1.0;
    const QSize device = deviceSizeOf(logicalPx, job.dpr);
    job.inputs.layout = plot::LayoutContext{device.width(), device.height(), static_cast<float>(job.dpr)};
    if (!job.inputs.scheme) {
        job.inputs.scheme = std::make_shared<const project::Scheme>(fonts_->scheme());
    }
    if (!job.inputs.metrics) {
        job.inputs.metrics = QtTextMetrics::create(*job.inputs.scheme, fonts_->generation());
    }
    job.builders = buildersFor(job.inputs.spec);
    if (device.isEmpty()) {
        return job;
    }
    TaskPool* pool = pool_ != nullptr ? pool_ : TaskPool::installed();
    if (pool != nullptr) {
        // An empty key never coalesces: every render is its own build (04 section 6.7 item 1).
        job.future = pool->submit(Priority::Visible, TaskKey{}, CancelToken::none(),
                                  [fn = job.builders.buildStatic,
                                   in = job.inputs](const CancelToken& cancel) { return fn(in, cancel); });
    } else {
        job.inlineResult = job.builders.buildStatic(job.inputs, CancelToken::none());
    }
    return job;
}

QImage OffscreenRenderer::finish(Job& job) const {
    const QSize device = deviceSizeOf(job.logical, job.dpr);
    if (device.isEmpty()) {
        return {};
    }
    Result<plot::ComponentBuild> built =
        job.future
            ? std::move(*job.future).take()
            : std::move(job.inlineResult).value_or(Result<plot::ComponentBuild>(plot::ComponentBuild{}));
    plot::DrawList staticList;
    if (built.ok()) {
        staticList = std::move(built).value().list;
    } else {
        staticList = placeholderList(job.inputs, "Render failed: " + built.error().message);
    }
    plot::DrawList dynamicList;
    plot::DrawList chromeList;
    try {
        dynamicList = job.builders.buildDynamic(job.inputs);
        chromeList = job.builders.buildChrome(job.inputs, false);
    } catch (const std::exception&) {
        dynamicList = emptyList(job.inputs);
        chromeList = emptyList(job.inputs);
    }

    LayerCache cache(fonts_);
    cache.resize(job.logical, job.dpr);
    cache.rasteriseStatic(staticList);
    cache.setDynamic(std::move(dynamicList));
    cache.setChrome(std::move(chromeList));

    return cache.composite();
}

QImage OffscreenRenderer::renderComponent(const plot::ComponentBuildInputs& inputs, QSize logicalPx,
                                          qreal dpr) {
    Job job = start(inputs, logicalPx, dpr);
    return finish(job);
}

QImage OffscreenRenderer::render(const project::WorkbookFile& workbook, int worksheet,
                                 const InputsProvider& inputs, QSize logicalPx, qreal dpr) {
    if (worksheet < 0 || static_cast<std::size_t>(worksheet) >= workbook.worksheets.size() ||
        logicalPx.isEmpty()) {
        return {};
    }
    dpr = dpr > 0.0 ? dpr : 1.0;
    const project::WorksheetDef& sheet = workbook.worksheets[static_cast<std::size_t>(worksheet)];
    std::vector<const project::ComponentSpec*> order;
    order.reserve(sheet.components.size());
    for (const project::ComponentSpec& c : sheet.components) {
        order.push_back(&c);
    }
    std::stable_sort(
        order.begin(), order.end(),
        [](const project::ComponentSpec* a, const project::ComponentSpec* b) { return a->z < b->z; });

    // Submit every Static build first so the pool works on them in parallel, then compose in z order.
    struct Placed {
        QRect rect;
        Job job;
    };
    std::vector<Placed> placed;
    placed.reserve(order.size());
    const int w = logicalPx.width();
    const int h = logicalPx.height();
    for (const project::ComponentSpec* spec : order) {
        const project::Rect& f = spec->rect;
        const int x0 = static_cast<int>(std::lround(f.l * w));
        const int y0 = static_cast<int>(std::lround(f.t * h));
        const int x1 = static_cast<int>(std::lround(f.r * w));
        const int y1 = static_cast<int>(std::lround(f.b * h));
        const QRect r(QPoint(x0, y0), QSize(std::max(0, x1 - x0), std::max(0, y1 - y0)));
        if (r.isEmpty()) {
            continue;
        }
        const QSize device = deviceSizeOf(r.size(), dpr);
        const plot::LayoutContext layout{device.width(), device.height(), static_cast<float>(dpr)};
        plot::ComponentBuildInputs in = inputs ? inputs(*spec, layout) : plot::ComponentBuildInputs{};
        if (!inputs) {
            in.spec = *spec;
        }
        placed.push_back(Placed{r, start(in, r.size(), dpr)});
    }

    QImage out(deviceSizeOf(logicalPx, dpr), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(detail::toQColor(fonts_->scheme().role("windowBackground", kWhite)));
    QPainter painter(&out);
    for (Placed& p : placed) {
        const QImage img = finish(p.job);
        if (!img.isNull()) {
            painter.drawImage(QPointF(p.rect.topLeft()), img);
        }
    }
    painter.end();
    return out;
}

} // namespace esses::qt
