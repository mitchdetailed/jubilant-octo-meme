// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OffscreenRenderer (04 section 6.7, D-033; AC-M1-04 goldens, AC-M1-05 render parity): renders a worksheet or
// one component to a QImage with the same builders the widgets use (plot::TimeDistanceBuilder and friends),
// submitting the Static builds to the TaskPool and waiting on the futures on its calling thread (allowed:
// only the interactive GUI thread carries ThreadRole::NoBlock, 04 section 1 item 4). Dynamic and Chrome lists
// are built on the calling thread. Each component is composed by a LayerCache exactly as its widget paints
// (Static image, then the Dynamic and Chrome lists), so the PNG equals the on-screen composite for the same
// inputs. Runs with QT_QPA_PLATFORM=offscreen; for determinism the caller registers the fonts first
// (SchemeFonts::registerApplicationFonts, tests/golden/fonts). The interactive Print to Clipboard does NOT
// use this class (it paints the component's current layers).
//
// Deviation from 04 section 6.7's signature: esses_qtrender cannot resolve maths outputs (it does not link
// esses_qtmodel or esses_maths), so render() takes an InputsProvider that turns each component spec into its
// prepared plot::ComponentBuildInputs (esses_qtmodel's RenderService::prepareInputs in esses-render and the
// tests). Builders are looked up by component type: setBuilders() overrides, else builtinBuilders()
// (time_distance -> plot::TimeDistanceBuilder; every other type, and every unknown component, renders the
// placeholder: background, frame and the type name).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QImage>
#include <QSize>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::qt {

// The three layer builders of one component type (04 sections 5.2 and 6.3).
struct ComponentBuilders {
    std::function<Result<plot::ComponentBuild>(const plot::ComponentBuildInputs&, const CancelToken&)>
        buildStatic;                                                               // worker
    std::function<plot::DrawList(const plot::ComponentBuildInputs&)> buildDynamic; // calling thread
    std::function<plot::DrawList(const plot::ComponentBuildInputs&, bool pending)> buildChrome;
};

class OffscreenRenderer {
public:
    // Inputs for one component at a layout (called on the renderer's calling thread).
    using InputsProvider = std::function<plot::ComponentBuildInputs(const project::ComponentSpec& spec,
                                                                    const plot::LayoutContext& layout)>;

    // `fonts` nullptr: the default SchemeFonts. `pool` nullptr: TaskPool::installed(), or the calling thread
    // when no pool is installed.
    explicit OffscreenRenderer(std::shared_ptr<const SchemeFonts> fonts, TaskPool* pool = nullptr);

    // Every component of worksheet `worksheet` at its fractional rectangle (rounded to logical pixels like
    // WorksheetCanvas), in ascending z order (stable), over the scheme's windowBackground. Null image for a
    // worksheet index out of range or an empty size.
    QImage render(const project::WorkbookFile& workbook, int worksheet, const InputsProvider& inputs,
                  QSize logicalPx, qreal dpr);
    // One component filling the image (device size logicalPx * dpr, DPR-tagged, transparent where the
    // component paints nothing). inputs.layout is replaced by the image's layout; a null scheme becomes the
    // fonts' scheme and null metrics a QtTextMetrics of it.
    QImage renderComponent(const plot::ComponentBuildInputs& inputs, QSize logicalPx, qreal dpr);

    void setBuilders(std::string type, ComponentBuilders builders);
    static ComponentBuilders builtinBuilders(std::string_view type);
    static ComponentBuilders placeholderBuilders();

private:
    struct Job;
    ComponentBuilders buildersFor(const project::ComponentSpec& spec) const;
    Job start(const plot::ComponentBuildInputs& inputs, QSize logicalPx, qreal dpr) const;
    QImage finish(Job& job) const;

    std::shared_ptr<const SchemeFonts> fonts_;
    TaskPool* pool_ = nullptr;
    std::map<std::string, ComponentBuilders, std::less<>> builders_;
};

} // namespace esses::qt
