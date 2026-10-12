// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers shared by the esses_qtrender Qt Tests (compiled into each tst_*.cpp executable): the test scheme,
// the golden fonts, synthetic DrawLists, pixel probes and the reference-image check.
//
// Reference images (libs/qtrender/tests/reference/<name>.png) hold geometry-only scenes: Qt's raster engine
// is platform independent for them, so they are compared on every OS with the golden rule (8/255 per channel,
// <= 0.5 % of the pixels). Text is checked by geometry (ink bounds against QtTextMetrics), not by pixels:
// glyph rasterisation differs between font engines, and pixel goldens with text are authoritative on ubuntu
// only (D-127). ESSES_UPDATE_REFERENCES=1 rewrites the references after a reviewed change. Every rendered
// image is also written to <build>/libs/qtrender/artefacts/ (with a diff image on failure).
#pragma once

#include <esses/plotcore/draw_list.hpp>
#include <esses/project/scheme.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QImage>
#include <QRect>
#include <QString>
#include <memory>

namespace esses::qttest {

QString repoRoot();
QString artefactDir();
QString referencePath(const QString& name);

// Before the QGuiApplication exists (the test classes' initMain()): QT_QPA_FONTDIR = tests/golden/fonts when
// unset, so a FreeType font database (the offscreen platform on Windows) sees only the golden fonts.
void prepareFontEnvironment();
// Registers tests/golden/fonts (Noto Sans) once; returns the number of font files.
int registerGoldenFonts();

// A scheme with Noto Sans in every role, distinct point sizes per role (so the style -> role mapping is
// observable) and the colour roles the renderer reads.
project::Scheme testScheme(double sizeOffset = 0.0);
std::shared_ptr<const qt::SchemeFonts> testFonts(double sizeOffset = 0.0);

// A list of a layer of logical size (w, h) at `dpr` (device size rounded).
plot::DrawList emptyList(int logicalW, int logicalH, float dpr);

// Rasterises `list` with RasterBackend into a fresh layer cleared to `clear`.
QImage rasterise(const plot::DrawList& list, plot::Rgba clear = 0xFFFFFFFFU,
                 std::shared_ptr<const qt::SchemeFonts> fonts = {});

// The geometry-only scene of the reference images: logical 240 x 160 at `dpr` (grid, bands, 1 px and 2 px
// traces, dashed line, points, lines, quads, scissor).
plot::DrawList geometryScene(float dpr);

// Straight-alpha pixel.
QRgb pixel(const QImage& img, int x, int y);
bool sameColour(QRgb a, QRgb b, int tolerance = 0);
// Bounding rectangle (device pixels) of the pixels differing from `background` by more than `tolerance`.
QRect inkBounds(const QImage& img, QRgb background, int tolerance = 16);

void saveArtefact(const QImage& img, const QString& name);
// Compares with the reference image; on failure writes <name>.actual.png and <name>.diff.png to the artefact
// directory and returns false with `message` set. ESSES_UPDATE_REFERENCES=1 writes the reference instead.
bool matchesReference(const QImage& img, const QString& name, QString& message);

} // namespace esses::qttest
