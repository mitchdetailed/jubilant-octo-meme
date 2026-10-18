// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Umbrella header of esses_analysis (layer 2, Qt-free; 01 section 2.1). M1 surface per 07 section 4.1; the M3
// contract (07 section 6.1) adds the track, report, binning and variance headers.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/analysis/beacon_pair_rule.hpp>
#include <esses/analysis/corrected_speed.hpp>
#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/fastest_lap.hpp>
#include <esses/analysis/histogram_binner.hpp>
#include <esses/analysis/lap_stretch.hpp>
#include <esses/analysis/range_rule.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/analysis/report_engine.hpp>
#include <esses/analysis/report_spec.hpp>
#include <esses/analysis/rule_runner.hpp>
#include <esses/analysis/scatter_binner.hpp>
#include <esses/analysis/section_detector.hpp>
#include <esses/analysis/stat_cache.hpp>
#include <esses/analysis/time_report_engine.hpp>
#include <esses/analysis/track_generator.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/analysis/track_sections_rule.hpp>
#include <esses/analysis/trend_fit.hpp>
#include <esses/analysis/variance_table.hpp>
#include <esses/analysis/whole_outing_rule.hpp>
