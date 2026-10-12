// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayerCache (04 section 6.2, D-058, D-137): one per ComponentWidget, GUI thread only. One cached Static
// QImage (device pixels, DPR-tagged) plus the GUI-built Dynamic and Chrome DrawLists, which are painted
// directly in paintEvent under the damage region (never cached as images: a full-size Dynamic image would
// cost ~200 MB of memory traffic per cursor move at 3,840 x 2,160). invalidate() follows the ChangeMask table
// of 04 section 2.2:
//   Cursor, Datum                       -> Dynamic
//   Zoom, Offsets, Scale, Size          -> Static + Dynamic
//   Domain, Selection, Data, Scheme     -> Static + Dynamic + Chrome
// A cursor or datum move calls QWidget::update(dynamicDamage(old, new)): the old and new cursor strips plus
// the key-value column, computed as the union of the device-pixel bounds of every command of both lists
// (strokes widened by w + 2 device px), in logical pixels. Memory rule: bytes() (Static image only) is summed
// into the Messages budget line; a hidden worksheet's component releases its image 30 s after hideEvent (the
// widget owns the timer).
// The Static image survives resize() and invalidate() (an in-flight rebuild keeps showing the previous image,
// 04 section 1 item 3); only releaseStatic() and the next storeStatic()/rasteriseStatic() replace it.
// The widget and OffscreenRenderer paint through the same paint(), which is what makes esses-render's PNG
// equal the on-screen composite (AC-M1-05).
#pragma once

#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QImage>
#include <QPainter>
#include <QRegion>
#include <QSize>
#include <cstddef>
#include <memory>

namespace esses::qt {

class LayerCache {
public:
    // `fonts` nullptr: the default SchemeFonts.
    explicit LayerCache(std::shared_ptr<const SchemeFonts> fonts = {});

    // Marks every layer dirty when the logical size or the DPR changes (the next Static image is allocated at
    // the new device size).
    void resize(QSize logical, qreal dpr);
    void invalidate(plot::ChangeMask mask);
    bool dirty(plot::LayerKind kind) const noexcept;
    void storeStatic(QImage image); // a worker DrawList rasterised by RasterBackend (image shared)
    // Rasterises a Static DrawList (RasterBackend::paintList) into the Static image, reusing the current
    // buffer when its size matches. Size: the list's widthPx x heightPx when set, else deviceSize(); DPR: the
    // list's. The layer is cleared to `clear` first (transparent by default; builders emit their background).
    void rasteriseStatic(const plot::DrawList& list, plot::Rgba clear = 0);
    void setDynamic(plot::DrawList list); // GUI-built
    void setChrome(plot::DrawList list);  // GUI-built
    void setFonts(std::shared_ptr<const SchemeFonts> fonts);
    // Old and new cursor/datum strips (x +- (w + 2) device px over the trace area) plus the key-value column,
    // in logical pixels, clipped to the component; empty when the lists are equal; the whole component when
    // either list holds a Clear.
    QRegion dynamicDamage(const plot::DrawList& before, const plot::DrawList& after) const;
    // paintEvent: blit the exposed part of the Static image, then paint the Dynamic and Chrome lists, all
    // clipped to `exposed` (logical pixels; an empty region paints nothing).
    void paint(QPainter& widgetPainter, const QRegion& exposed) const;
    // The current composite (Static image, Dynamic and Chrome lists) as a DPR-tagged image of deviceSize(),
    // transparent where nothing is painted: Print to Clipboard of the component's current layers (04 section
    // 7.2 item 9) and OffscreenRenderer. Null before resize().
    QImage composite() const;
    void releaseStatic();               // hidden worksheet, 30 s after hideEvent
    std::size_t bytes() const noexcept; // Static image only
    const QImage& staticImage() const noexcept { return static_; }
    const plot::DrawList& dynamicList() const noexcept { return dynamic_; }
    const plot::DrawList& chromeList() const noexcept { return chrome_; }
    QSize logicalSize() const noexcept { return logical_; }
    qreal devicePixelRatio() const noexcept { return dpr_; }
    QSize deviceSize() const noexcept; // logicalSize() * devicePixelRatio(), rounded per dimension

private:
    const SchemeFonts& fonts() const;

    std::shared_ptr<const SchemeFonts> fonts_;
    QSize logical_;
    qreal dpr_ = 1.0;
    QImage static_;
    plot::DrawList dynamic_;
    plot::DrawList chrome_;
    bool staticDirty_ = true;
    bool dynamicDirty_ = true;
    bool chromeDirty_ = true;
};

} // namespace esses::qt
