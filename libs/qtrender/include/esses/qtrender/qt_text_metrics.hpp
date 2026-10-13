// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// QtTextMetrics (04 section 5.4): plot::ITextMetrics over immutable font descriptions. Constructed on the GUI
// thread from the active scheme after every application font is registered; holds only plain values (one font
// description per TextStyleId, from the same role table as SchemeFonts, and the generation). No QFont is ever
// shared between threads: each call uses a thread_local cache of QFont + QFontMetricsF keyed by (snapshot id,
// style); the snapshot id is process-unique, so two snapshots that share a generation never share a cache
// entry. A scheme change creates a new snapshot with a new generation; in-flight builds keep the old one
// through their shared_ptr. Test: tests/tst_text_metrics_concurrency.cpp (8 threads while the GUI swaps
// schemes).
// Device pixels: the logical metric (QFontMetricsF at the default 96-dpi-per-DPR logical DPI) times dpr,
// which is exactly how RasterBackend lays text out (logical font under the painter's DPR scale), so a
// builder's measurement and the rasterised run agree at every DPR. Line height is ascent + descent (no
// leading).
#pragma once

#include <esses/plotcore/text_metrics.hpp>
#include <esses/project/scheme.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace esses::qt {

class QtTextMetrics final : public plot::ITextMetrics {
public:
    struct FontDescription {
        std::string family;
        double pointSize = 9.0;
        int weight = 400;
        bool italic = false;
        bool rotated = false;
    };
    static constexpr std::size_t kStyles = plot::FixedTextMetrics::kStyles;

    QtTextMetrics(const project::Scheme& scheme, std::uint64_t generation);
    static std::shared_ptr<const QtTextMetrics> create(const project::Scheme& scheme,
                                                       std::uint64_t generation);

    float advancePx(plot::TextStyleId style, std::string_view utf8, float dpr) const override;
    float lineHeightPx(plot::TextStyleId style, float dpr) const override;
    float ascentPx(plot::TextStyleId style, float dpr) const override;
    std::uint64_t generation() const override { return generation_; }

    const FontDescription& description(plot::TextStyleId style) const;

private:
    std::array<FontDescription, kStyles> fonts_{};
    std::uint64_t generation_ = 0;
    std::uint64_t id_ = 0; // per-thread cache key (process-unique)
};

} // namespace esses::qt
