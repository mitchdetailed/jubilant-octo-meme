// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The built-in module esses.gps_corrections v1 (03 section 9.2, D-016, D-024, 07 section 5.1). Registered for
// every source on open whenever its latitude/longitude names resolve (through mappings and aliases); missing
// lat/lon gives status Disabled (no GPS channels), never an error. From M2 only step 1 runs, always, whatever
// `enabled` says: samples where latitude and longitude are both exactly 0 are invalid on both channels. Its
// outputs shadow the inputs (self-maths, outputs "shadow"), so every consumer (track generation, the Lap
// Editor GPS gate, KML export) reads the chain head without the (0, 0) fixes. Steps 2-4 (duplicate removal,
// time shift, status channel) and the Corrected GPS dialog are v1.0. Outputs are F64.
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/builtin_module.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

class GpsCorrectionsModule final : public IBuiltinModule {
public:
    static constexpr std::string_view kId = "esses.gps_corrections";
    static constexpr int kVersion = 1;
    static constexpr std::string_view kSettingsSchemaId = "builtin-esses.gps_corrections-1";
    static constexpr std::string_view kSettingsSchemaFile = "builtin-esses.gps_corrections-1.schema.json";

    GpsCorrectionsModule() = default;

    std::string id() const override { return std::string(kId); }
    int version() const override { return kVersion; }
    std::string settingsSchemaId() const override { return std::string(kSettingsSchemaId); }
    const model::OpaqueJson& settingsSchema() const override { return schema_; }
    // The settings object of 03 section 9.2 (enabled false, outputs "shadow"), canonical.
    model::OpaqueJson defaultSettings() const override;
    // Two outputs, F64, with the zero-fix validity step: named like the latitude and longitude inputs
    // (outputs "shadow", self-maths) or with {"prefix": text} prepended. Both require the two inputs
    // (Disabled "Disabled (no GPS channels)" without them). Settings errors: E-GPS-SETTINGS (context.detail =
    // pointer).
    Result<std::vector<BuiltinOutput>> expand(const model::OpaqueJson& settings,
                                              const units::UnitRegistry& units,
                                              Diagnostics& diags) const override;
    // The zero-fix step runs whatever the builtin item's `enabled` says (it gates steps 2-4 only, v1.0).
    bool disabledItemDisablesOutputs() const override { return false; }

    // As CorrectedSpeedDistanceModule::loadSchema.
    Result<void> loadSchema(const std::filesystem::path& schemasDir);

private:
    model::OpaqueJson schema_;
};

} // namespace esses::maths
