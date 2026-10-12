// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemeFonts (04 section 6.1 item 4, 06 section 16): the scheme's font roles as QFonts, and the mapping of
// the plot text styles onto them: Axis -> axis (axisRotated when quarterTurns = 1), Caption and Banner ->
// caption, Key and KeyValue -> key, Band, Measurement and GaugeUnits -> small, Gauge -> gauge, Table ->
// reportTable, Equation -> equation, ColourBand -> colourBands, TrackLabel -> trackLabel. Fonts use
// QFont::NoSubpixelAntialias for determinism. A missing role falls back to the scheme's caption font, then to
// project::FontSpec{} (Noto Sans 9 pt); a family that is not installed goes through Qt family substitution
// (06 section 16 rule 6).
// The object holds only the Qt-free scheme; QFonts are built per call (font(), role()) or kept in per-thread
// caches keyed by id() (RasterBackend::paintList), so one SchemeFonts may be read from several threads.
// registerApplicationFonts() must run before any QtTextMetrics exists (04 section 5.4); the golden tests
// register tests/golden/fonts/ only (04 section 6.7 item 2).
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/project/scheme.hpp>

#include <QFont>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace esses::qt {

class SchemeFonts {
public:
    SchemeFonts();
    explicit SchemeFonts(const project::Scheme& scheme, std::uint64_t generation = 1);

    // The font for a text style. Point sizes are logical: the painter's device pixel ratio applies.
    QFont font(plot::TextStyleId style, bool rotated = false) const;
    // A scheme font role by name ("valuesItem"); the caption font when the role is missing.
    QFont role(const QString& name) const;
    // The scheme font role a text style uses (04 section 6.1 item 4).
    static std::string_view roleName(plot::TextStyleId style, bool rotated = false) noexcept;
    std::uint64_t generation() const noexcept { return generation_; }
    // Process-unique id of this font set (copies share it: same scheme, same fonts); keys per-thread caches.
    std::uint64_t id() const noexcept { return id_; }
    const project::Scheme& scheme() const noexcept { return scheme_; }

    // Registers every .ttf/.otf of `dir` (sorted by file name) with QFontDatabase (start-up and
    // OffscreenRenderer set-up; needs a QGuiApplication); returns the number of font files registered,
    // counting files this application registered before. 0 for a missing directory or without a
    // QGuiApplication.
    static int registerApplicationFonts(const std::filesystem::path& dir);

private:
    project::Scheme scheme_;
    std::uint64_t generation_ = 1;
    std::uint64_t id_ = 0;
};

} // namespace esses::qt
