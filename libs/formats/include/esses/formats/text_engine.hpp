// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TextEngine (05 section 7.1, D-029): the light two-pass engine behind every text importer. Pass 1 (open,
// worker Interactive) detects the encoding, builds the line index, applies the profile's preamble, header,
// time and lap-source rules, infers the rate (the rational with the smallest denominator <= 1,000 within
// 0.1 % of 1/step, uniform when every sampled row is within 0.6/rate, else timestamped), classifies the
// columns and, for sparse "hold", builds the slab carry-in table; it produces the ProbeResult. Pass 2
// (readMany) decodes 65,536-row slabs, one sweep per slab shared by every column of the source (share key
// slab/<sourceId>/<k>), with fast_float for numbers (decimal comma included). The readers are IImporters,
// so the registry, the CLI and SourceLoader treat them like the .ld reader.
//
// Importer ids: the shipped profiles give "motec.csv" (kLegacyCsvImporterId, csv.hpp) and
// "text.profile:generic.csv" (kGenericCsvImporterId); a user profile gives "text.profile:<id>".
#pragma once

#include <esses/base/result.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/text_profile.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace esses::formats {

class TextEngine {
public:
    // An importer reading files described by `profile` (id = profile.effectiveImporterId()), applying
    // `dictionary` (may be null) before the channels are built.
    static std::unique_ptr<IImporter> makeImporter(TextProfile profile,
                                                   std::shared_ptr<const Dictionary> dictionary = nullptr);
    // Every <dir>/*.json profile, in file-name order; unreadable or invalid files are skipped and logged.
    static std::vector<TextProfile> loadProfiles(const std::filesystem::path& dir,
                                                 std::vector<std::string>& log);
};

} // namespace esses::formats
