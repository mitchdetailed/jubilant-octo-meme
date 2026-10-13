// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The CSV importers and writers (05 section 6 and 7.3; D-028, D-044 moved them to M2). Reading is the
// TextEngine with the shipped profiles data/profiles-text/motec.csv.json (importer id motec.csv, kept as the
// D-028 built-in id) and data/profiles-text/generic.csv.json (text.profile:generic.csv, the auto-detecting
// fallback). The legacy CSV writer emits the observed skeleton of 05 section 6 (metadata lines, names row,
// units row, every field double-quoted, CRLF, '.' decimal, single rate, invalid samples as ""); the generic
// writer writes a plain delimited table. Identifiers stay neutral (00 section 8 rule 1); the importer ids are
// the allow-listed literals.
#pragma once

#include <esses/formats/importer.hpp>
#include <esses/units/unit_registry.hpp>

#include <memory>
#include <string_view>

namespace esses::formats {

inline constexpr std::string_view kLegacyCsvImporterId = "motec.csv";
inline constexpr std::string_view kGenericCsvProfileId = "generic.csv";
inline constexpr std::string_view kGenericCsvImporterId = "text.profile:generic.csv";
// Writer ids: the legacy writer shares the importer id; the plain table writer is "csv" (05 section 9.2).
inline constexpr std::string_view kLegacyCsvWriterId = kLegacyCsvImporterId;
inline constexpr std::string_view kGenericCsvWriterId = "csv";
// The literal format name the legacy CSV's first line carries (05 section 6 writer item 6; vendor-mark
// allow-list class c, tools/vendor_marks_allowlist.txt).
inline constexpr std::string_view kLegacyCsvFormatName = "MoTeC CSV File";

// Writers (export, project-aware CSV with maths in esses-cli export). `units` is borrowed for unit labels and
// the display-unit option {"csv": {"units": {"<channel>": "<unit>"}}}, and may be null (base units, no unit
// labels). ImporterRegistry::withBuiltins registers both.
std::unique_ptr<IWriter> makeLegacyCsvWriter(const units::UnitRegistry* units = nullptr);
std::unique_ptr<IWriter> makeGenericCsvWriter(const units::UnitRegistry* units = nullptr);

} // namespace esses::formats
