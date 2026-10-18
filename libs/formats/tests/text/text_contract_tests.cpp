// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The parts of the M2 text-importer contract that were final before the implementations landed (07 section
// 5.1, 05 sections 7.2 and 8.2): importer ids and the unit-map file stem. The implementations are tested by
// the other files of this directory (one Catch2 executable, formats_text.<test>).

#include <esses/formats/csv.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/formats/unit_mapping_profile.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace esses::formats;

TEST_CASE("text profiles: effective importer id", "[unit]") {
    TextProfile generic;
    generic.id = std::string(kGenericCsvProfileId);
    CHECK(generic.effectiveImporterId() == kGenericCsvImporterId);
    TextProfile shipped;
    shipped.id = std::string(kLegacyCsvImporterId);
    shipped.importerId = std::string(kLegacyCsvImporterId);
    CHECK(shipped.effectiveImporterId() == kLegacyCsvImporterId);
}

TEST_CASE("unit maps: file stem of an importer id", "[unit]") {
    CHECK(UnitMappingProfile::fileStem(kGenericCsvImporterId) == "generic.csv");
    CHECK(UnitMappingProfile::fileStem("text.profile:efi-ascii") == "efi-ascii");
    CHECK(UnitMappingProfile::fileStem(kLegacyCsvImporterId) == kLegacyCsvImporterId);
}

TEST_CASE("text profiles: defaults of the importer-profile-1 key table", "[unit]") {
    const TextProfile p;
    CHECK(p.delimiter == "auto");
    CHECK(p.decimal == "auto");
    CHECK(p.quote == '"');
    CHECK(p.layout == TextProfile::Layout::Table);
    CHECK(p.sparse == TextProfile::Sparse::Timestamped);
    CHECK(p.sniff.confidence == 50);
    CHECK(p.header.ignoreEmptyNames);
    CHECK(p.lapSource.kind == "none");
}
