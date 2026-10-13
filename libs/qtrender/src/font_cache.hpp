// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal to esses_qtrender: the QFont construction rule shared by SchemeFonts and QtTextMetrics (so layout
// measurement and rasterisation see identical fonts), the TextStyleId -> scheme font role table of 04 section
// 6.1 item 4, and the per-thread caches (04 section 5.4: no QFont is ever shared between threads).
//
// Lifetime: every cache is thread_local. Workers of the TaskPool are joined before the application object is
// destroyed (AppRuntime::shutdown), so their caches die while the font machinery is alive. The thread that
// owns the QCoreApplication would destroy its thread_locals only after main() returns, i.e. after the
// QGuiApplication tore the font database down; its caches are therefore cleared by a post routine
// (qAddPostRoutine, which ~QGuiApplication runs before it releases the fonts).
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/project/scheme.hpp>

#include <QFont>
#include <QFontMetricsF>
#include <QStaticText>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace esses::qt::detail {

// 04 section 6.1 item 4: Axis -> axis (axisRotated when rotated), Caption and Banner -> caption, Key and
// KeyValue -> key, Band, Measurement and GaugeUnits -> small, Gauge -> gauge, Table -> reportTable, Equation
// -> equation, ColourBand -> colourBands, TrackLabel -> trackLabel.
std::string_view roleForStyle(plot::TextStyleId style, bool rotated) noexcept;

// family, pointSizeF, weight (CSS scale 1..1000, the scale Qt 6 uses), italic; NoSubpixelAntialias for
// determinism (04 section 6.1 item 4). Point sizes are logical: the painter's device pixel ratio applies.
QFont makeFont(const std::string& family, double pointSize, int weight, bool italic);
inline QFont makeFont(const project::FontSpec& spec) {
    return makeFont(spec.family, spec.pointSize, spec.weight, spec.italic);
}

// Process-unique ids for font sets (SchemeFonts, QtTextMetrics instances): the per-thread caches are keyed by
// (set id, slot), never by an address or by a generation alone (two snapshots may share a generation).
std::uint64_t nextFontSetId() noexcept;

struct FontEntry {
    QFont font;
    QFontMetricsF metrics; // logical pixels at the default (screen / QImage) DPI
};

// The calling thread's cached font for (setId, slot), or nullptr.
const FontEntry* findThreadFont(std::uint64_t setId, std::uint32_t slot);
// Inserts (or replaces) the calling thread's font for (setId, slot); the reference stays valid until the next
// insert on this thread.
const FontEntry& insertThreadFont(std::uint64_t setId, std::uint32_t slot, const QFont& font);

// The calling thread's QStaticText LRU (04 section 6.1 item 4: keyed by text, style and dpr; 4,096 entries).
// `key` must identify the font set, the slot, the dpr and the (elided) text.
QStaticText& threadStaticText(const std::string& key, const QString& text);
std::size_t threadStaticTextCount();
inline constexpr std::size_t kStaticTextCapacity = 4096;

} // namespace esses::qt::detail
