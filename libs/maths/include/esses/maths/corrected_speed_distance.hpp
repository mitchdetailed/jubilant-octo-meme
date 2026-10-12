// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The built-in module esses.corrected_speed_distance v1 (03 section 9.1, D-016; AC-M1-10: Corr Speed and
// Corr Dist are produced through PlanProvider as this module, prefetched at Background after open, and the
// module's settings validate against its schema; AC-M1-11 for the numbers). Registered for every source on
// open; settings come from project.json builtinMaths["esses.corrected_speed_distance"], per-source overrides
// from the sidecar. Outputs: "Corr Speed" (Speed, m/s, F32, Linear) and "Corr Dist" (Distance, m, F64, reads
// Corr Speed as stored). The algorithms are analysis::CorrectedSpeed and analysis::CorrectedDistance.
//
// expand(): the settings (null = defaultSettings()) are validated against the settings schema when it is
// loaded (E-CSD-SETTINGS with the JSON pointer), then parsed by analysis::CorrectedSpeedSettings::fromJson.
// Corr Speed reads the method's channels (fastestWheel: its two channels, then the alternate list it falls
// back to; alternateList: the list; gCorrection: speed and long-G channels) through
// analysis::CorrectedSpeed::compute; its diagnostics become Warning diagnostics (W-CSD-FALLBACK) and the
// channels used an Info diagnostic (I-CSD-INPUTS). Corr Dist integrates Corr Speed
// (analysis::CorrectedDistance::integrate) or, with distance.source "logged", unwraps the logged lap
// distance channel converted to metres (CorrectedDistance::fromLogged).
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/builtin_module.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

class CorrectedSpeedDistanceModule final : public IBuiltinModule {
public:
    static constexpr std::string_view kId = "esses.corrected_speed_distance";
    static constexpr int kVersion = 1;
    static constexpr std::string_view kSettingsSchemaId = "builtin-esses.corrected_speed_distance-1";
    // The settings schema file, relative to the schemas directory (<exe dir>/data/schemas, or schemas/ in the
    // source tree): schemas/builtin-esses.corrected_speed_distance-1.schema.json.
    static constexpr std::string_view kSettingsSchemaFile =
        "builtin-esses.corrected_speed_distance-1.schema.json";
    static constexpr std::string_view kSpeedOutput = "Corr Speed";
    static constexpr std::string_view kDistanceOutput = "Corr Dist";

    CorrectedSpeedDistanceModule() = default;

    std::string id() const override { return std::string(kId); }
    int version() const override { return kVersion; }
    std::string settingsSchemaId() const override { return std::string(kSettingsSchemaId); }
    const model::OpaqueJson& settingsSchema() const override { return schema_; }
    // The settings object of 03 section 9.1 (method fastestWheel), canonical.
    model::OpaqueJson defaultSettings() const override;
    Result<std::vector<BuiltinOutput>> expand(const model::OpaqueJson& settings,
                                              const units::UnitRegistry& units,
                                              Diagnostics& diags) const override;

    // Loads the settings schema from `schemasDir` (Io when the file is missing or unreadable, Format when it
    // is not a draft-07 schema with $id urn:esses:<kSettingsSchemaId>); until then settingsSchema() is null
    // and expand() relies on CorrectedSpeedSettings::fromJson alone.
    Result<void> loadSchema(const std::filesystem::path& schemasDir);
    // validateSettings(*this, settings): NotFound until loadSchema() succeeded.
    Result<void> validateSettings(const model::OpaqueJson& settings) const;
    // The first directory that holds kSettingsSchemaFile: `dataDir`/schemas (esses_base Paths::dataDir(),
    // when not empty), <exe dir>/data/schemas, then the source tree's schemas/ (developer builds). Empty when
    // none does.
    static std::filesystem::path findSchemasDir(const std::filesystem::path& dataDir = {});

private:
    model::OpaqueJson schema_;
};

} // namespace esses::maths
