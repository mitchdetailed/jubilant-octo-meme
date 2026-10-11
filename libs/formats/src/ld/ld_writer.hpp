// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LdWriter: the .ld part of the fixture writer (05 section 5; docs/formats/ld-converter-port.md section 1 and
// the inverse mapping of section 3). New code written from the specification (D-144).
//
// Layout emitted: the 1,762-byte header, then the event, venue, vehicle and weather records at their fixed
// sizes and contiguous (every pointer a u32), the device-event block (header 0x688) when events are given,
// the enumeration block (header 0x690) when tables are given, the channel records (124 bytes, prev/next
// linked) each followed by its gap block when it has invalid samples and its aux block when it has a long
// name or an enumeration reference, an optional terminator record, then the sample arrays. The u32 at 0x5DE
// is always written as 0 (D-143). Dialect options set the dialect-specific fields for fixtures. The writer
// writes stored (raw) values: quantisation from engineering values is the caller's job (quantiseRaw helps).
#pragma once

#include "ld/ld_file.hpp"

#include <esses/base/result.hpp>
#include <esses/formats/import_log.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::formats::ld {

enum class WriteDialect : std::uint8_t { PcLayout, M1, CSeries, SimExporter, ConverterLegacy };
std::string_view writeDialectName(WriteDialect d) noexcept; // "pc-layout", "m1", ...

struct LdWriteChannel {
    std::string name;      // UTF-8; written to the 32-byte field (cut on a code-point boundary when longer)
    std::string longName;  // written as an aux long name when non-empty; defaults to `name` when the encoded
                           // name exceeds 32 bytes
    std::string shortName; // C-series dialect: the 8-byte short name at 0x40
    std::string unit;      // 20 bytes at 0x40 (12 bytes at 0x48 in the C-series dialect)
    std::uint16_t typeCode = layout::kTypeFloat;
    std::uint16_t width = 4;
    std::uint16_t rateHz = 1;
    std::int16_t shift = 0, mul = 1, scale = 1, dec = 0;
    std::uint8_t dpsByte = 0;
    std::uint8_t interpByte = layout::kInterpLinear;
    std::optional<std::pair<float, float>> displayRange; // written (lo, hi) at 0x5C / 0x60
    std::optional<std::uint16_t> counter;                // default 20101 + index
    std::optional<std::uint16_t> enumRef;                // type-5 aux argument
    bool longNamePadByte = false;                        // the converter's extra 0 byte after the long name

    // Stored samples: exactly one of these is used. rawData is written verbatim (count = size / width).
    std::vector<std::int64_t> rawInts; // integer type pairs
    std::vector<double> rawFloats;     // float type pairs (F16 rounded to nearest even)
    std::vector<std::byte> rawData;

    std::vector<LdInterval> invalid; // written as gap-block entry/exit pairs, in this order
    // Hostile fixtures: gap events written verbatim instead of `invalid` (kind, sample index).
    std::optional<std::vector<std::pair<std::uint16_t, std::uint32_t>>> rawGapEvents;
    std::optional<std::int32_t> rawMax, rawMin; // default: max/min of the valid stored values
    std::optional<std::uint32_t> declaredCount; // count written in the record (default: samples)
    std::optional<std::array<std::byte, layout::kRecordSize>> rawRecord; // verbatim base record (round trip)
};

struct LdWriteDetails {
    std::string date = "01/01/2026", time = "00:00:00"; // DD/MM/YYYY, 24 h HH:MM:SS
    std::string driver, vehicleId, engineId, venue, session, shortComment, team;
    std::string eventName, eventSession, longComment; // eventSession empty: the session again (Q-W10)
    std::string venueName, venueCategory;
    std::uint32_t venueLengthMm = 0;
    std::string vehicleRecordId, vehicleDesc, vehicleEngineId, vehicleType, driveType, vehicleComment,
        vehicleNumber;
    std::uint16_t weightKg = 0, fuelTankX10 = 0, diffX1000 = 0, trackMm = 0, wheelbaseMm = 0;
    std::array<std::uint16_t, layout::kGearCount> gearsX1000{};
    std::string sky, windDirection, weatherComment;
    LdWeatherValue airTemp, trackTemp, baroPressure, relHumidity, windSpeed;
};

struct LdWriteEvent {
    std::uint16_t code = 0;
    double timeUs = 0.0;
};

struct LdWriteInput {
    LdWriteDetails details;
    std::vector<LdWriteChannel> channels;
    std::vector<LdEnumTable> enumTables;
    std::vector<LdWriteEvent> events;
};

struct LdWriteOptions {
    WriteDialect dialect = WriteDialect::PcLayout;
    // Device identity; default per dialect (pc-layout: Esses' neutral constants).
    std::optional<std::string> deviceType;
    std::optional<std::uint32_t> serial;
    std::optional<std::uint16_t> deviceVersion;
    // Sample arrays start on this boundary (1 = contiguous, as the converter and M1 files).
    std::uint32_t dataAlignment = 64;
    // M1 terminator record; default: dialect M1.
    std::optional<bool> terminator;
    // Zero region before the event record; default 0x10000 for C-series (the logger-file layout), else 0.
    std::optional<std::uint32_t> configRegionBytes;
    // Override of the u16 at 0x56 (count-mismatch fixtures).
    std::optional<std::uint16_t> headerChannelCount;
    // Verbatim round trip (05 section 5 item 8): the header and detail records of a read file. Unknown bytes
    // are kept; pointers, counts and 0x5DE..0x5E1 (always 0, D-143) are rewritten, the pointers to blocks the
    // writer does not copy (0x004, 0x68C) are zeroed, and the text fields are kept as stored.
    std::vector<std::byte> rawHeader; // 0x6E2 bytes, or empty
    std::vector<std::byte> rawEvent, rawVenue, rawVehicle, rawWeather;
};

class LdWriter {
public:
    // The file image. Notes: ld.truncated, ld.string_encoding (UTF-8 used), ld.value_clamped.
    static Result<std::vector<std::byte>> build(const LdWriteInput& input, const LdWriteOptions& options = {},
                                                ImportLog* notes = nullptr);
    // build() + AtomicFile (temp + rename, 05 section 1.3 rule 10). Returns the bytes written.
    static Result<std::uint64_t> write(const std::filesystem::path& path, const LdWriteInput& input,
                                       const LdWriteOptions& options = {}, ImportLog* notes = nullptr);

    // Everything an LdFile holds as writer input: channels with their raw records, raw sample bytes and gap
    // intervals; enumeration tables; device events; details. With optionsFromFile() it gives the
    // read -> write -> read round trip of 05 section 5 item 8.
    static Result<LdWriteInput> inputFromFile(const LdFile& file);
    static LdWriteOptions optionsFromFile(const LdFile& file);
};

// Engineering value -> stored integer for an encoding (the inverse of the reader's scaling), rounded to
// nearest; for fixtures and future export.
std::int64_t quantiseRaw(double value, std::int16_t shift, std::int16_t mul, std::int16_t scale,
                         std::int16_t dec, ShiftOrder order) noexcept;

} // namespace esses::formats::ld
