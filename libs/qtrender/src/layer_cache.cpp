// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "paint_internal.hpp"

#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/raster_backend.hpp>

#include <QRect>
#include <QRectF>
#include <cmath>
#include <utility>

namespace esses::qt {

LayerCache::LayerCache(std::shared_ptr<const SchemeFonts> fonts) : fonts_(std::move(fonts)) {}

const SchemeFonts& LayerCache::fonts() const {
    return fonts_ ? *fonts_ : detail::defaultSchemeFonts();
}

void LayerCache::resize(QSize logical, qreal dpr) {
    if (logical == logical_ && dpr == dpr_) {
        return;
    }
    logical_ = logical;
    dpr_ = dpr > 0.0 ? dpr : 1.0;
    staticDirty_ = dynamicDirty_ = chromeDirty_ = true;
}

QSize LayerCache::deviceSize() const noexcept {
    return QSize(static_cast<int>(std::lround(logical_.width() * dpr_)),
                 static_cast<int>(std::lround(logical_.height() * dpr_)));
}

void LayerCache::invalidate(plot::ChangeMask mask) {
    if ((mask & plot::kDynamicOnly) != 0) {
        dynamicDirty_ = true;
    }
    if ((mask & plot::kStaticRebuild) != 0) {
        staticDirty_ = dynamicDirty_ = true;
    }
    if ((mask & plot::kFullRebuild) != 0) {
        staticDirty_ = dynamicDirty_ = chromeDirty_ = true;
    }
}

bool LayerCache::dirty(plot::LayerKind kind) const noexcept {
    switch (kind) {
    case plot::LayerKind::Static:
        return staticDirty_;
    case plot::LayerKind::Dynamic:
        return dynamicDirty_;
    case plot::LayerKind::Chrome:
        return chromeDirty_;
    }
    return true;
}

void LayerCache::storeStatic(QImage image) {
    QImage previous = std::exchange(static_, std::move(image));
    if (previous.size() == static_.size()) {
        detail::recycleStaticBuffer(std::move(previous)); // the next worker raster of this size reuses it
    }
    staticDirty_ = false;
}

void LayerCache::rasteriseStatic(const plot::DrawList& list, plot::Rgba clear) {
    const QSize device =
        list.widthPx > 0 && list.heightPx > 0 ? QSize(list.widthPx, list.heightPx) : deviceSize();
    if (device.isEmpty()) {
        storeStatic(QImage());
        return;
    }
    QImage img;
    if (static_.size() == device && static_.format() == QImage::Format_ARGB32_Premultiplied) {
        img = std::move(static_); // reuses the buffer unless someone else still shares it
        static_ = QImage();
    } else {
        img = QImage(device, QImage::Format_ARGB32_Premultiplied);
    }
    img.setDevicePixelRatio(list.dpr > 0.0F ? static_cast<qreal>(list.dpr) : dpr_);
    // A list that starts with a Clear of the whole layer (every builder's background) replaces each pixel
    // anyway; skipping the fill saves one pass over the image.
    const bool clearsFirst = !list.cmds.empty() && list.cmds.front().kind == plot::CmdKind::Clear &&
                             QSize(list.widthPx, list.heightPx) == device;
    if (!clearsFirst) {
        img.fill(detail::toQColor(clear));
    }
    detail::paintStaticList(img, list, fonts());
    storeStatic(std::move(img));
}

void LayerCache::setDynamic(plot::DrawList list) {
    dynamic_ = std::move(list);
    dynamicDirty_ = false;
}

void LayerCache::setChrome(plot::DrawList list) {
    chrome_ = std::move(list);
    chromeDirty_ = false;
}

void LayerCache::setFonts(std::shared_ptr<const SchemeFonts> fonts) {
    fonts_ = std::move(fonts);
}

QRegion LayerCache::dynamicDamage(const plot::DrawList& before, const plot::DrawList& after) const {
    if (before == after) {
        return {};
    }
    QRect bounds(QPoint(0, 0), logical_);
    if (bounds.isEmpty()) { // not laid out yet: the lists' own layer rectangles
        for (const plot::DrawList* l : {&before, &after}) {
            const qreal dpr = l->dpr > 0.0F ? static_cast<qreal>(l->dpr) : 1.0;
            bounds |= QRectF(0.0, 0.0, l->widthPx / dpr, l->heightPx / dpr).toAlignedRect();
        }
    }
    QRegion region;
    for (const plot::DrawList* l : {&before, &after}) {
        const qreal dpr = l->dpr > 0.0F ? static_cast<qreal>(l->dpr) : 1.0;
        for (const plot::DrawCmd& cmd : l->cmds) {
            const detail::CommandBounds b = detail::commandBounds(*l, cmd, fonts());
            if (b.wholeLayer) {
                return QRegion(bounds);
            }
            if (b.rect.isEmpty()) {
                continue;
            }
            const QRectF logical(b.rect.x() / dpr, b.rect.y() / dpr, b.rect.width() / dpr,
                                 b.rect.height() / dpr);
            region += logical.toAlignedRect().intersected(bounds);
        }
    }
    return region;
}

void LayerCache::paint(QPainter& widgetPainter, const QRegion& exposed) const {
    if (exposed.isEmpty()) {
        return;
    }
    widgetPainter.save();
    widgetPainter.setClipRegion(exposed, widgetPainter.hasClipping() ? Qt::IntersectClip : Qt::ReplaceClip);
    if (!static_.isNull()) {
        widgetPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        widgetPainter.drawImage(QPointF(0.0, 0.0), static_);
    }
    RasterBackend::paintList(widgetPainter, dynamic_, fonts());
    RasterBackend::paintList(widgetPainter, chrome_, fonts());
    widgetPainter.restore();
}

QImage LayerCache::composite() const {
    const QSize device = deviceSize();
    if (device.isEmpty()) {
        return {};
    }
    QImage out(device, QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr_);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    paint(painter, QRegion(QRect(QPoint(0, 0), logical_)));
    painter.end();
    return out;
}

void LayerCache::releaseStatic() {
    static_ = QImage();
    staticDirty_ = true;
}

std::size_t LayerCache::bytes() const noexcept {
    return static_cast<std::size_t>(static_.sizeInBytes());
}

} // namespace esses::qt
