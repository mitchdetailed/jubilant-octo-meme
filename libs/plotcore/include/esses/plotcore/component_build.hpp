// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The inputs and outputs of a component build (04 section 5.5: "each builder is a pure function of
// (ComponentSpec, ViewState, SelectionSnapshot, data snapshot, LayoutContext, const ITextMetrics&)"; 04
// section 6.3; 01 section 5.3 rules 2 and 9). Added in M1 so that the widget (esses_widgets ComponentWidget
// via RenderService) and OffscreenRenderer / esses-render run the very same builders (04 section 7.2 item 9,
// AC-M1-05) without either depending on the other.
//
// ComponentBuildInputs is a value: every member is a copy or a std::shared_ptr<const ...>, so it can be moved
// into a worker task. Channel names are resolved before the build (logged channels through the
// ChannelResolver, maths outputs such as Corr Speed / Corr Dist through MathsService; esses_qtmodel prepares
// the inputs), so plotcore never links esses_maths (D-048).
#pragma once

#include <esses/model/channel.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/text_metrics.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/project/scheme.hpp>
#include <esses/store/series.hpp>
#include <esses/units/converter.hpp>
#include <esses/units/unit_registry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::plot {

struct ResolvedChannel {
    std::string name;                                       // as the component references it
    std::optional<model::ChannelDescriptor> descriptor;     // nullopt: missing in this slot's source
    std::optional<store::Series> series;                    // nullopt: missing or still pending
    model::ChannelStatus status = model::ChannelStatus::Ok; // Pending: "..." / hatching (D-022)
    units::Converter toDisplay;                             // base -> display unit (D-010)
    std::string displayUnit;                                // symbol for labels
    int dps = 2;
};

struct SlotData {
    Slot slot;
    std::shared_ptr<const model::DataSource> source;
    std::string layer = "Normal";
    RangeGeom range;
    OutingGeom outing;
    std::shared_ptr<const DistanceChain> chain; // null: no distance data for this source (banner, AC-M1-09)
    std::vector<ResolvedChannel> channels;      // in the component's channel order
};

struct ComponentBuildInputs {
    project::ComponentSpec spec;
    ViewState view;
    SelectionSnapshot selection;
    std::vector<SlotData> slots; // SelectionSnapshot::visibleSlots() order, Main first
    std::shared_ptr<const project::Scheme> scheme;
    std::shared_ptr<const ITextMetrics> metrics;
    LayoutContext layout;
    bool focused = false; // Chrome
    // Added in M1: converts manual scales persisted with a unit ({v, unit}, 04 section 8.14) to the axis
    // unit. Null: a manual scale is read in the axis display unit when its unit matches, else in base units.
    std::shared_ptr<const units::UnitRegistry> units;
    // Added in M1: the TaskPool the Static build fans its per-trace decimation out to (04 section 6.3 item
    // 2). Null: TaskPool::currentWorkerPool(), else the traces are decimated in order on the calling thread.
    TaskPool* pool = nullptr;
};

struct ComponentBuild {
    DrawList list;
    HitTester hits; // Static builds only
    std::uint64_t viewVersion = 0;
};

} // namespace esses::plot
