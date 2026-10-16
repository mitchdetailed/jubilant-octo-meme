// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Mapping-panel hand-off of the open pipeline (05 sections 2 and 8.1, D-028, D-029; 01 section 5.1 rule W
// (d): no worker ever waits for the user). When step 4 leaves a channel with an unknown unit or name and the
// caller passed a MappingHook, task A ends: the hook packages the unresolved rows and the opaque OpenState
// skeleton into a PendingResolution, which the load Future delivers (ImporterRegistry::load returns a
// LoadResult). SourceLoader posts it to the GUI, the MappingPanel shows it, and the user's MappingDecision is
// submitted by value as task B through ImporterRegistry::resume. Cancel drops the PendingResolution.
//
// M2 contract deviation from 05 section 2: MappingDecision::unitMap is a typed UnitMappingProfile (05 writes
// JsonDoc), and LoadResult (registry.hpp) carries an OpenedSource rather than the bare DataSource.
#pragma once

#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/model/resolver.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::formats {

struct OpenState; // opaque state task A hands to task B: reader, probe, skeleton DataSource, ResolutionTraces

struct UnresolvedRow {
    std::string loggedName;
    std::string vendorUnit;
    std::optional<std::string> inferredQuantity;
    std::vector<double> sampleValues; // a few decoded values for the panel's "Sample values" column
};

struct PendingResolution { // the end of task A
    std::string path;      // the file being opened (UTF-8), for the panel and Messages
    std::string importerId;
    std::vector<UnresolvedRow> rows;
    std::shared_ptr<const OpenState> skeleton;
};

struct MappingDecision {
    std::vector<model::MappingRow> mappings; // analysis names (project Mappings, D-013)
    std::optional<UnitMappingProfile> unitMap;
    bool saveUnitMap = false; // write channels/unitmaps/<importerStem>.json
    bool openAnyway = false;  // unresolved rows open unitless with status text
};

// Packages the unresolved rows and the skeleton; never waits (called on the worker running task A).
using MappingHook = std::function<PendingResolution(std::vector<UnresolvedRow> rows,
                                                    std::shared_ptr<const OpenState> skeleton)>;

} // namespace esses::formats
