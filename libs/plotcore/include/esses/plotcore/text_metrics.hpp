// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ITextMetrics (04 section 5.4): text measurement for layout (key column width, tick-label collision, caption
// eliding). Contract: every const member is safe to call concurrently from any thread; builders receive a
// std::shared_ptr<const ITextMetrics> captured by the task (01 section 5.3 rule 9). esses_qtrender implements
// it as QtTextMetrics; FixedTextMetrics is the test double (0.6 em advance per code point, 1.2 em line
// height, em = style point size * dpr * 96 / 72) that layout tests and the DrawList goldens use.
#pragma once

#include <esses/plotcore/draw_list.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace esses::plot {

class ITextMetrics {
public:
    virtual ~ITextMetrics() = default;
    virtual float advancePx(TextStyleId style, std::string_view utf8, float dpr) const = 0;
    virtual float lineHeightPx(TextStyleId style, float dpr) const = 0;
    virtual float ascentPx(TextStyleId style, float dpr) const = 0;
    virtual std::uint64_t generation() const = 0; // scheme/font generation the snapshot was made for

protected:
    ITextMetrics() = default;
    ITextMetrics(const ITextMetrics&) = default;
    ITextMetrics& operator=(const ITextMetrics&) = default;
};

class FixedTextMetrics final : public ITextMetrics {
public:
    static constexpr std::size_t kStyles = 13; // number of TextStyleId values

    // Point size per style; the default is 8 pt for every style except Caption/Banner (9 pt) and Gauge (16
    // pt).
    FixedTextMetrics();
    explicit FixedTextMetrics(const std::array<float, kStyles>& pointSizes, std::uint64_t generation = 1);

    float advancePx(TextStyleId style, std::string_view utf8, float dpr) const override;
    float lineHeightPx(TextStyleId style, float dpr) const override;
    float ascentPx(TextStyleId style, float dpr) const override;
    std::uint64_t generation() const override { return generation_; }

    float emPx(TextStyleId style, float dpr) const noexcept;

private:
    std::array<float, kStyles> pointSizes_{};
    std::uint64_t generation_ = 1;
};

} // namespace esses::plot
