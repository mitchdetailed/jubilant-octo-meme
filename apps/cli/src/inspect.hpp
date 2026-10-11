// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli info | channels | laps (05 section 9.2, 07 section 3.1) and the JSON documents behind them, which
// fixture add / verify reuse:
//   info      sniff + probe; --json prints probe_json (probe-1, canonical). With importer options the probe
//             comes from the reader that open() returns (the options can change it, e.g. ld.shiftOrder);
//             without, from IImporter::probe (O(header), AC-M0-12).
//   channels  the channel table after the open pipeline (units resolved to the quantity's base unit unless
//             --no-units; vendor unit from the probe).
//   laps      markers (.ld device events, .ldx markers, synthesised SOL/EOL) and the range-group tree of the
//             phase-1 rules (M0: whole-outing ranges); --json prints laps-1. --no-sidecar gives the
//             expected.laps.json view.
#pragma once

#include "app_context.hpp"

#include <esses/base/result.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/probe.hpp>
#include <esses/formats/registry.hpp>
#include <esses/model/source.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace esses::cli {

struct InspectOptions {
    std::filesystem::path file;
    bool json = false;
    std::string importerId;  // --importer: skip sniffing
    std::string optionsJson; // --options: ImportOptions::options, e.g. {"ld":{"shiftOrder":"beforeMul"}}
    bool noSidecar = false;  // channels, laps
    bool timing = false;     // info: probe time on stderr
};

// ImportOptions from an importer id ("" = sniff) and the options as JSON text ("" = none) or value.
Result<formats::ImportOptions> makeImportOptions(std::string_view importerId, std::string_view optionsJson);
Result<formats::ImportOptions> makeImportOptionsJson(std::string_view importerId,
                                                     const nlohmann::json& options);

struct ProbeOutcome {
    formats::ProbeResult probe;
    std::string importerId;
    int confidence = 0; // 0 when the importer was named
    std::string reason;
    double millis = 0.0; // sniff + probe
};
Result<ProbeOutcome> probeFile(AppContext& ctx, const std::filesystem::path& file,
                               const formats::ImportOptions& options);

// The open pipeline on the calling thread.
Result<formats::OpenedSource> openFile(AppContext& ctx, const std::filesystem::path& file,
                                       const formats::ImportOptions& options, bool withUnits,
                                       bool loadSidecar);

// markers-1 marker record (02 section 5.3 rule 8): id, class, group, tUs, name, comment, flags (omitted when
// 0), prov.
nlohmann::json markerJson(const model::Marker& marker);
// laps-1 of a source: every layer's enabled markers and its range-group tree.
nlohmann::json lapsJson(const model::DataSource& source);
// The channels table as JSON (no schema): {"file", "importerId", "layers": {"Normal": [rows]}}.
nlohmann::json channelsJson(const formats::OpenedSource& opened, const units::UnitRegistry* units);

int runInfo(AppContext& ctx, const InspectOptions& options, std::ostream& out, std::ostream& err);
int runChannels(AppContext& ctx, const InspectOptions& options, std::ostream& out, std::ostream& err);
int runLaps(AppContext& ctx, const InspectOptions& options, std::ostream& out, std::ostream& err);

// "Io: cannot open x" style one-line message of an error.
std::string describe(const Error& error);

} // namespace esses::cli
