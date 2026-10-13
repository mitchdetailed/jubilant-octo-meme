// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the component Qt Tests (tests/components/tst_*.cpp; compiled into every esses_widgets test
// executable, so everything lives in esses::widgets::cmptest): the golden fonts, the shipped units and light
// scheme, synthetic circuit sources built in memory (an out lap, flying laps, an in lap; wheel speeds whose
// integral is exactly the track length per lap, so the built-in Corr Speed / Corr Dist and the distance
// indexes exist; a throttle channel in %), time_distance and values_list specs, and a World with the
// qtmodel services, one worksheet and its WorksheetContext.
#pragma once

#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QImage>
#include <QRect>
#include <QString>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace esses::widgets::cmptest {

std::filesystem::path repoRoot();
// Before the QApplication exists (initMain): QT_QPA_FONTDIR = tests/golden/fonts unless set.
void prepareFontEnvironment();
// Registers tests/golden/fonts once (needs the QApplication).
int registerGoldenFonts();
std::shared_ptr<const units::UnitRegistry> shippedUnits();
// data/schemes/<name>.scheme.json.
std::shared_ptr<const project::Scheme> shippedScheme(const std::string& name = "light");

struct SourceOptions {
    std::string path = "graph.ld";
    std::vector<double> lapSeconds{40.0, 50.0, 48.0, 49.0, 30.0}; // out lap, flying laps, in lap
    double trackLengthM = 2000.0;
    unsigned rateHz = 50;
    bool wheelSpeeds = true; // "Wheel Speed FL/FR" (m/s); false: no distance data
    double throttlePhase = 0.0;
};
std::shared_ptr<const model::DataSource> makeSource(const SourceOptions& options, store::ChunkCache& cache);
model::RangeRef lapRef(const model::DataSource& src, std::size_t index);
const model::Range& lapAt(const model::DataSource& src, std::size_t index);

// A time_distance component: one group per entry of `groups` (channel names), default display.
project::ComponentSpec timeDistanceSpec(const std::vector<std::vector<std::string>>& groups,
                                        project::Rect rect = {}, int z = 0, const std::string& id = {});
project::ComponentSpec valuesListSpec(const std::vector<std::string>& channels, project::Rect rect = {},
                                      int z = 0, const std::string& id = {});

// The qtmodel services of one workspace with one worksheet, and its WorksheetContext.
struct World {
    // markGuiThreadNoBlock: the calling (GUI) thread carries ThreadRole::NoBlock (AC-M1-12); the caller
    // resets the role after the World is gone.
    explicit World(const std::string& domain = "time", bool markGuiThreadNoBlock = false);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    std::shared_ptr<const model::DataSource> publish(const SourceOptions& options);
    // Corr Dist chains of every lap of `src` are ready (distance mode draws).
    bool distanceReady(const std::shared_ptr<const model::DataSource>& src);

    qt::AppRuntime rt;
    qt::MathsService maths;
    qt::DistanceService distance;
    qt::DataSelectionModel selection;
    qt::WorkspaceViewModel workspace;
    qt::RenderService render;
    qt::CommandRegistry commands;
    project::WorksheetDef sheet;
    std::unique_ptr<qt::WorksheetViewModel> view;
    std::shared_ptr<const project::Scheme> scheme;
    std::unique_ptr<WorksheetContext> context;
};

// Pixel helpers (straight alpha).
QRgb pixelAt(const QImage& img, int x, int y);
// Number of pixels in `rect` (device px) whose RGB equals `rgba` (0xRRGGBBAA, alpha ignored).
int countColour(const QImage& img, const QRect& rect, project::Rgba rgba);
// The colours of the DrawList's Polyline commands (the traces) in emission order.
std::vector<project::Rgba> strokeColours(const plot::DrawList& list);

} // namespace esses::widgets::cmptest
