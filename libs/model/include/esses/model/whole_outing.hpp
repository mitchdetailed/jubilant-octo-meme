// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The whole-outing range rule (02 section 4.5 "circuit.whole-outing", 00 glossary "Outing"): one Range of
// kind Outing per logged session of a layer, in group "Outings". It is the only rule M0 needs ('esses-cli
// laps' lists whole-outing ranges until the beacon-pair rule arrives in M1, 07 section 3); esses_analysis'
// WholeOutingRule (M1) reads its rule-1 file and calls this function, so the two cannot drift.
//
// Semantics (phase 1: markers and details only, never a sample). Over the enabled intrinsic markers (group
// "") in model order: SOL opens an outing; the next EOL closes it; a SOL while one is open closes the open
// outing at the new SOL; an EOL with none open is ignored; an outing still open at the end closes at the
// layer duration with a nil end marker. A layer without any SOL gets one outing from 0 to its first EOL (else
// to max(duration, last marker t)), anchored on the id an importer-synthesised SOL at t = 0 would have, so
// ids stay stable either way. Ranges are numbered from 1 in time order; name = the name detail (default 'File
// Name'), abbrev = the outing index, trusted = options.trusted; id = rangeId("Outings", start marker).
#pragma once

#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>

#include <string>
#include <vector>

namespace esses::model {

struct DataSource;

struct WholeOutingOptions {
    std::string groupPath = "Outings";
    std::string nameDetail = "File Name"; // the rule's "name": "'File Name'"
    std::string fallbackName = "Outing";  // when the detail is absent or empty
    bool trusted = true;
    bool supportsTrackGeneration = false;
    bool supportsReporting = true;
};

// The "Outings" group of one layer (version 0; the caller's tree assigns versions).
RangeGroup buildWholeOutingGroup(const DataLayer& layer, const std::vector<Detail>& details,
                                 const WholeOutingOptions& options = {});

// Phase 1 for M0, every layer of `src`: replaces the layer's "Outings" root (children are kept), applies the
// sidecar's range overrides, writes nothing back to the document, and carries range-group versions over from
// the previous tree (D-140).
SidecarApplyReport rebuildWholeOutingRanges(DataSource& src, const WholeOutingOptions& options = {});

} // namespace esses::model
