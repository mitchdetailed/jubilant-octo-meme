// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal helpers of the motec.ld importer and writer: the bridge between the .ld/.ldx core (src/ld/, which
// includes nothing above esses_store) and esses_model's types.
//   * LdSourceEncoding::Origin: the parsed file and channel index behind a channel the reader produced.
//   * The raw-header blob of IReader::rawHeader() / SourceProvenance::rawHeader: the 1,762 header bytes
//     (0x5DE..0x5E1 held as 0, D-143), the tag "ESR1", then the event, venue, vehicle and weather records,
//     each as a u32 little-endian length (0 = absent) and its bytes.
//   * Details: LdFile -> model::Detail (05 sections 3.2 and 3.3) and back to LdWriteDetails.
//   * The .ldx companion: LdxDocument -> model::CompanionData and DataSource -> LdxDocument, with the
//     companionOpaque encoding described in ldx_companion.cpp.
#pragma once

#include "ld/ld_file.hpp"
#include "ld/ld_writer.hpp"
#include "ld/ldx.hpp"

#include <esses/base/time.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::units {
class UnitRegistry;
} // namespace esses::units

namespace esses::formats {

struct LdSourceEncoding::Origin {
    std::shared_ptr<const ld::LdFile> file;
    std::uint32_t channel = 0;
};

namespace ldimpl {

LdEncoding encodingOf(const ld::LdChannel& c);

// ---- raw header blob --------------------------------------------------------------------------------------
std::vector<std::byte> packRawHeader(const ld::LdFile& file);
struct RawHeaderParts {
    std::vector<std::byte> header, event, venue, vehicle, weather;
};
// nullopt when the blob is not one packRawHeader produced.
std::optional<RawHeaderParts> unpackRawHeader(std::span<const std::byte> blob);

// ---- details ----------------------------------------------------------------------------------------------
// The header and detail-record details in the order of 05 sections 3.2 and 3.3 (prov Logged). Empty text
// and zero numbers produce no detail; fixed-unit numbers are in the quantity's base unit with its symbol
// ("m", "kg", "m3", "ratio"); a weather value is a number with the unit text as written when it parses, else
// text.
std::vector<model::Detail> detailsOf(const ld::LdFile& file);
// The layer's UTC anchor: the header date and time read as UTC (05 section 3.7, utcAssumedLocal).
std::optional<UtcInstant> utcOf(const ld::LdHeader& header);
// The inverse for a fresh header: text details as they are, numbers converted to the record's fixed units.
// `units` (may be null) resolves a symbol that is not a base-unit symbol; a detail that cannot be written
// leaves a note.
ld::LdWriteDetails writeDetailsOf(const std::vector<model::Detail>& details, const units::UnitRegistry* units,
                                  ImportLog& notes);

// ---- .ldx companion ---------------------------------------------------------------------------------------
model::CompanionData companionFromLdx(const ld::LdxDocument& doc);
// The .ldx of one layer for the export writer (05 section 5 item 7): the structure and unknown elements kept
// in companionOpaque, every Companion and User marker that is not a device event (an ignored BCN or SPLTBCN
// as IGRDBCN), and every Companion and User detail.
ld::LdxDocument ldxFromSource(const model::DataSource& source, const model::DataLayer& layer, bool markers,
                              bool details);
// True when the document holds anything worth a file (markers, details, unknown elements or kept structure).
bool ldxWorthWriting(const ld::LdxDocument& doc, const model::DataSource& source);

} // namespace ldimpl
} // namespace esses::formats
