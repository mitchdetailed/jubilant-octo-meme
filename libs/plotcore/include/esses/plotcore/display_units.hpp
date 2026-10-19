// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The unit a channel is shown in (D-010, D-211 (a); docs/milestones/M3.md section 6 item 19). Samples are
// stored in the SI base unit of their quantity; a component names the unit a channel is shown in
// (time_distance and outing traces, values_list rows, the gauges, the scatter's x, y and colour channels and
// the channel report's channels carry an optional displayUnit). Where none is named, the channel is shown in
// its quantity's default display unit: the stored unit, except that a fraction stored in the Ratio base unit
// "ratio" is shown in "%" (a logged throttle, brake or duty reads 0-100, not 0-1). The logged unit as the
// default (the reference behaviour) needs the channel descriptor to keep the file's unit and is v1.0's.
// esses_qtmodel's channel resolution (RenderService::prepareInputs, ValuesModel, ReportModel) applies this
// rule, so every component, the Values window and the reports agree. Pure functions of the registry.
#pragma once

#include <esses/units/unit_registry.hpp>

#include <string_view>

namespace esses::plot {

// The unit a channel stored in `stored` is shown in when nothing names one: "%" for the Ratio quantity's base
// unit, else `stored` itself (also for an id the registry does not hold).
units::UnitId defaultDisplayUnit(const units::UnitRegistry& registry, units::UnitId stored);

// The unit a channel stored in `stored` is shown in: `requested` when it names a unit of the stored unit's
// quantity (symbol or alias, as UnitRegistry::find within the quantity), else defaultDisplayUnit.
units::UnitId displayUnitFor(const units::UnitRegistry& registry, units::UnitId stored,
                             std::string_view requested);

// The Corr Dist display unit (03 section 9.1 item 6: it follows the speed unit, km/h -> km, mph -> mi, else
// m, and the Channel Editor overrides it; 06 section 13.5 item 4: the Track Editor's section bounds are in
// it): `distanceOverride` (the project's channel override of Corr Dist) when it names a unit of the Length
// quantity, else km when `speedUnit` (the Corr Speed override) is km/h, mi when it is mph, else m. The
// registry's m when it has no Length quantity.
units::UnitId corrDistDisplayUnit(const units::UnitRegistry& registry, std::string_view distanceOverride,
                                  std::string_view speedUnit);

} // namespace esses::plot
