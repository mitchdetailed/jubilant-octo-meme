// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The motec.ld importer and writer (05 sections 3 to 5): the IImporter over the .ld/.ldx core of src/ld/ and
// the export-only IWriter that writes .ld + .ldx (fixtures in M0, File > Export Data at v1.0).
//
// Importer options (ImportOptions::options, schema from optionsSchema()):
//   {"ld": {"shiftOrder": "afterMul" | "beforeMul", "enumScanFallback": true | false,
//           "gapConvention": "standard" | "shifted"}}
// shiftOrder defaults to afterMul (D-037 as amended by spike S-1); enumScanFallback (default true) allows the
// enumeration-table scan when the 0x690 pointer is 0 (05 section 3.9); gapConvention (default standard)
// reads gap-block pairs as [entry, exit), or with "shifted" as [entry - 1, exit + 1) for files written by
// the converter's Python writer (ld.gap_convention_shifted, port specification section 1.8). Unknown members
// are Invalid.
//
// Writer options (WriterOptions::options):
//   {"ld": {"dialect": "pc-layout" | "m1" | "c-series" | "sim-exporter" | "converter-legacy",
//           "dataAlignment": 1 | 2 | 4 | ... | 4096, "verbatim": true | false}}
// verbatim (default true): channels read from a .ld are written with their stored samples, gap blocks,
// records and the source's header and detail records unchanged (05 section 5 items 2 and 8); an explicit
// dialect writes a fresh header from the details instead. Channels without an .ld origin are quantised with
// their LdSourceEncoding, or with the default encoding of their dtype (05 section 5 item 2).
#pragma once

#include <esses/formats/importer.hpp>
#include <esses/model/channel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace esses::units {
class UnitRegistry;
} // namespace esses::units

namespace esses::formats {

inline constexpr std::string_view kLdImporterId = "motec.ld";
inline constexpr std::string_view kLdImporterVersion = "0.1.0";

// The record fields that decide how one channel is stored in a .ld (05 section 3.4, port specification
// section 1.7). The reader fills every field from the record; a fixture generator sets what it needs.
struct LdEncoding {
    std::uint16_t typeCode = 0x07; // dtype_a
    std::uint16_t width = 4;       // bytes per sample
    std::uint16_t rateHz = 1;
    std::int16_t shift = 0, mul = 1, scale = 1, dec = 0;
    std::uint8_t dpsByte = 0;             // display decimals byte at record 0x64
    std::uint8_t interpByte = 3;          // 0 closest, 1 previous, 2 next, 3 linear
    std::optional<std::uint16_t> counter; // default on write: 20101 + index
    std::string name;                     // the 32-byte name field as decoded
    std::string longName;                 // aux long name, "" when none
    std::string unit;                     // vendor unit text as stored
    std::string shortName;                // C-series 8-byte short name
    std::optional<std::uint16_t> enumRef;
    std::optional<std::pair<float, float>> displayRange;
    std::optional<std::array<std::byte, 124>> rawRecord; // the record as read (round trips)

    friend bool operator==(const LdEncoding&, const LdEncoding&) = default;
};

// model::SourceEncoding of a .ld channel (02 section 4.1). A channel the reader produced also carries its
// origin (the parsed file and its index there; opaque outside esses_formats), so the writer can copy the
// stored samples and gap blocks verbatim.
class LdSourceEncoding final : public model::SourceEncoding {
public:
    struct Origin;

    explicit LdSourceEncoding(LdEncoding encoding, std::shared_ptr<const Origin> origin = nullptr);

    std::string_view importerId() const override { return kLdImporterId; }
    const LdEncoding& encoding() const noexcept { return encoding_; }
    const std::shared_ptr<const Origin>& origin() const noexcept { return origin_; }

private:
    LdEncoding encoding_;
    std::shared_ptr<const Origin> origin_;
};

// `units` (borrowed, may be null) answers the C-series unit rule (c): does the 8-byte field name a unit?
// Without it the field stays the short name (05 section 3.4).
std::unique_ptr<IImporter> makeLdImporter(const units::UnitRegistry* units = nullptr);
// `units` (borrowed, may be null) converts base-unit samples back to a channel's vendor unit when a channel
// without an .ld origin must be quantised in that unit.
std::unique_ptr<IWriter> makeLdWriter(const units::UnitRegistry* units = nullptr);

} // namespace esses::formats
