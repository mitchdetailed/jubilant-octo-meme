// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LdFile: the parsed metadata of one .ld file over a MappedFile (or an in-memory buffer), and random-access
// sample decode (05 section 3; docs/formats/ld-converter-port.md section 3, steps R1-R12).
//
// Parsing is O(header): the header, the four detail records reached through their pointers, the device-event
// block (0x688), the enumeration block (0x690), the channel record list with each record's gap and aux
// blocks. No sample is read. Every pointer check widens to 64 bits; the walk is bounded by size / 124 records
// and a visited set; every heuristic leaves a code in the ImportLog (ld_codes.hpp).
//
// An LdFile is immutable after parse and safe to share across threads; decode() is stateless and may run
// concurrently for different ranges (05 section 1.3 rule 5).
#pragma once

#include "ld/ld_decode.hpp"

#include <esses/base/mapped_file.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/ld/ld_layout.hpp>
#include <esses/store/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace esses::formats::ld {

// Dialect flags (05 section 3.8), listed in ProbeResult::dialect in this order.
namespace dialect {
inline constexpr std::uint32_t kM1 = 1u << 0;
inline constexpr std::uint32_t kCSeries = 1u << 1;
inline constexpr std::uint32_t kPcLayout = 1u << 2;
inline constexpr std::uint32_t kConverterLegacy = 1u << 3;
inline constexpr std::uint32_t kSimExporter = 1u << 4;
inline constexpr std::uint32_t kProLogging = 1u << 5;
} // namespace dialect
// "m1,pro-logging"; "" when no flag is set.
std::string dialectString(std::uint32_t flags);

// ChannelInfo::flags bits (05 section 1.2).
namespace channel_flag {
inline constexpr std::uint32_t kVendorDerived = 1u << 0;
inline constexpr std::uint32_t kHasDisplayRange = 1u << 1;
inline constexpr std::uint32_t kStatusBits = 1u << 2;
inline constexpr std::uint32_t kUnitFromShortName = 1u << 3;
inline constexpr std::uint32_t kDtypeGuessed = 1u << 4;
} // namespace channel_flag

// How gap-block event pairs (entry, exit) are read (port specification section 1.8). Standard: [entry, exit),
// the converter's C++ writer and M1 files (a reversed pair is read as [exit, entry)). Shifted: the
// converter's Python writer, (first invalid + 1, last invalid), read as [entry - 1, exit + 1).
enum class GapConvention : std::uint8_t { Standard, Shifted };

struct LdOptions {
    ShiftOrder shiftOrder = ShiftOrder::AfterMul; // ld.shiftOrder (D-037 as amended 2026-10-10)
    // Unit rule (c) on C-series files: does the 8-byte string resolve in the unit registry? Empty: never
    // (the string stays the short name, Info ld.unit_from_short_name_candidate).
    std::function<bool(std::string_view)> unitResolves;
    // i3rs-style signature scan for enumeration tables when the 0x690 pointer is 0 (05 section 3.9).
    bool enumScanFallback = true;
    GapConvention gapConvention = GapConvention::Standard; // ld.gapConvention
};

struct LocalDateTime { // header date + time as written (no zone; 05 section 3.7 utcAssumedLocal)
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    friend bool operator==(const LocalDateTime&, const LocalDateTime&) = default;
};

struct LdHeader {
    std::uint32_t magic = 0;
    std::uint32_t unknownPtr004 = 0;
    std::uint32_t metaPtr = 0;
    std::uint32_t dataPtr = 0;
    std::uint32_t eventPtr = 0;
    std::uint16_t unknown040 = 0;
    std::uint32_t ticks = 0;
    std::uint32_t serial = 0;
    std::string deviceType;
    std::uint16_t deviceVersion = 0;
    std::uint16_t unknown054 = 0;
    std::uint16_t channelCount = 0;  // u16 at 0x56
    std::uint16_t channelCount2 = 0; // u16 at 0x58
    std::uint16_t unknown05A = 0;
    std::uint16_t unknown05C = 0;
    std::string date, time, driver, vehicleId, engineId, venue, session, shortComment, team;
    bool proLoggingNonZero = false; // the only thing recorded about 0x5DE (D-143)
    std::uint8_t unknown687 = 0;
    std::uint32_t deviceEventPtr = 0;
    std::uint32_t unknownPtr68C = 0;
    std::uint32_t enumPtr = 0;
    std::optional<LocalDateTime> dateTime;
};

struct LdEventRecord {
    std::uint64_t offset = 0;
    std::string name, session, longComment;
    std::uint32_t venuePtr = 0, weatherPtr = 0;
};
struct LdVenueRecord {
    std::uint64_t offset = 0;
    std::string name;
    std::uint32_t lengthMm = 0;
    std::uint32_t vehiclePtr = 0;
    std::string category;
};
struct LdVehicleRecord {
    std::uint64_t offset = 0;
    std::string id, desc, engineId;
    std::uint16_t weightKg = 0, fuelTankX10 = 0;
    std::string type, driveType;
    std::uint16_t diffX1000 = 0;
    std::array<std::uint16_t, layout::kGearCount> gearsX1000{};
    std::uint16_t trackMm = 0, wheelbaseMm = 0;
    std::string comment, number;
};
struct LdWeatherValue {
    std::string value, unit;
    friend bool operator==(const LdWeatherValue&, const LdWeatherValue&) = default;
};
struct LdWeatherRecord {
    std::uint64_t offset = 0;
    std::string sky;
    LdWeatherValue airTemp, trackTemp, baroPressure, relHumidity, windSpeed;
    std::string windDirection, comment;
};

// One record of the log-event block (0x688): DeviceEvent markers (05 section 3.7).
struct LdDeviceEvent {
    std::uint16_t code = 0;
    double timeUs = 0.0;           // as stored (fractional on M1 files)
    Micros tUs = 0;                // llround(timeUs)
    std::uint32_t storedIndex = 0; // position in the block (events are ordered by (tUs, storedIndex))
    std::uint32_t ordinal = 0;     // index among events with the same tUs (02 section 4.3 rule 5)
};
// Esses' own wording for codes 0-5, "Event <code>" otherwise (port specification section 1.5; no vendor
// text).
std::string deviceEventLabel(std::uint16_t code);

struct LdEnumEntry {
    std::int16_t value = 0;
    std::string label;
    bool extended = false;      // a severity block follows the value in the file
    std::uint16_t severity = 0; // 0 normal, 1 info, 2 warning, 3 fault
    friend bool operator==(const LdEnumEntry&, const LdEnumEntry&) = default;
};
struct LdEnumTable {
    std::uint16_t index = 0; // the number a channel's aux block names (never the position)
    std::vector<LdEnumEntry> entries;
    std::uint64_t offset = 0;
};

// Half-open run [begin, end) of invalid samples.
struct LdInterval {
    std::uint64_t begin = 0, end = 0;
    friend bool operator==(const LdInterval&, const LdInterval&) = default;
};

// The on-disk encoding of one channel record (port specification section 4, LdChannelEncoding): the decoded
// fields and the 124 raw bytes, so a writer can re-emit the record verbatim.
struct LdChannelEncoding {
    std::uint64_t recordOffset = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t storedCount = 0; // u32 at 0x0C as stored
    std::uint32_t prevPtr = 0, nextPtr = 0, gapPtr = 0, auxPtr = 0;
    std::uint16_t counter = 0, typeCode = 0, bytesPerSample = 0, rateHz = 0;
    std::int16_t shift = 0, mul = 0, scale = 0, dec = 0;
    std::int32_t rawMax = 0, rawMin = 0;
    float rangeA = 0.0f, rangeB = 0.0f;
    std::uint8_t dpsByte = 0, interpByte = 0;
    std::array<std::byte, layout::kRecordSize> raw{};
};

struct LdChannel {
    std::uint32_t index = 0; // position among the channels (terminator and skipped records excluded)
    std::string loggedName;  // long name when present, else the 32-byte name; ' (n)' suffix on duplicates
    std::string name32;      // the 32-byte name field, decoded
    std::string longName;    // from the aux block; "" when none
    std::string shortName;   // C-series 8-byte short name (unit rule (b)); else ""
    std::string unit;        // vendor unit text (unit rule)
    std::string vendorId;    // the record counter, decimal
    SampleType sampleType = SampleType::F32;
    store::Dtype dtype = store::Dtype::F32;
    store::SampleKind kind = store::SampleKind::Real;
    store::Interp interp = store::Interp::Linear;
    std::uint64_t sampleCount = 0; // stored count, clamped to the file
    int dps = 0;
    std::uint32_t flags = 0;                             // channel_flag bits
    std::optional<std::uint16_t> enumRef;                // table index named by the aux block
    bool enumResolved = false;                           // the table exists; kind is Enum
    std::optional<std::pair<float, float>> displayRange; // (lo, hi) in vendor units
    bool longNamePadByte = false; // the converter's extra byte after the long name was seen
    ScalingPlan scaling;
    std::vector<LdInterval> invalid; // sorted, merged, within [0, sampleCount)
    LdChannelEncoding enc;

    Rate rate() const noexcept { return Rate::fromHz(enc.rateHz); }
    std::string_view vendorDtype() const noexcept { return vendorDtypeName(sampleType); }
};

class LdFile {
    struct PrivateTag {};

public:
    static Result<std::shared_ptr<const LdFile>> open(const std::filesystem::path& path,
                                                      LdOptions options = {});
    static Result<std::shared_ptr<const LdFile>> fromMapped(std::shared_ptr<const MappedFile> file,
                                                            LdOptions options = {});
    // In-memory bytes (writer round trips, fuzzing, companions).
    static Result<std::shared_ptr<const LdFile>>
    fromBuffer(std::shared_ptr<const std::vector<std::byte>> bytes, LdOptions options = {});

    explicit LdFile(PrivateTag) {}
    LdFile(const LdFile&) = delete;
    LdFile& operator=(const LdFile&) = delete;

    std::span<const std::byte> bytes() const noexcept { return bytes_; }
    const MappedFile* mapped() const noexcept { return mapped_.get(); }
    const std::filesystem::path& path() const noexcept { return path_; }
    const LdOptions& options() const noexcept { return options_; }

    const LdHeader& header() const noexcept { return header_; }
    // The 1,762 header bytes with 0x5DE..0x5E1 held as 0 (D-143).
    std::span<const std::byte> rawHeader() const noexcept { return rawHeader_; }
    const std::optional<LdEventRecord>& eventRecord() const noexcept { return event_; }
    const std::optional<LdVenueRecord>& venueRecord() const noexcept { return venue_; }
    const std::optional<LdVehicleRecord>& vehicleRecord() const noexcept { return vehicle_; }
    const std::optional<LdWeatherRecord>& weatherRecord() const noexcept { return weather_; }
    // Raw bytes of each present detail record (its fixed size, or what fits in the file).
    std::span<const std::byte> rawEventRecord() const noexcept { return rawEvent_; }
    std::span<const std::byte> rawVenueRecord() const noexcept { return rawVenue_; }
    std::span<const std::byte> rawVehicleRecord() const noexcept { return rawVehicle_; }
    std::span<const std::byte> rawWeatherRecord() const noexcept { return rawWeather_; }

    const std::vector<LdDeviceEvent>& deviceEvents() const noexcept { return events_; }
    const std::vector<LdEnumTable>& enumTables() const noexcept { return enums_; }
    // The first table with this index (a later duplicate logs ld.enum_index_duplicate); O(1).
    const LdEnumTable* findEnumTable(std::uint16_t index) const noexcept;
    const std::vector<LdChannel>& channels() const noexcept { return channels_; }

    std::uint32_t dialect() const noexcept { return dialect_; }
    std::string dialectString() const { return ld::dialectString(dialect_); }
    std::string_view stringEncoding() const noexcept { return stringEncoding_; } // ascii|utf-8|cp1252|mixed
    bool headerOnly() const noexcept { return headerOnly_; }
    std::uint32_t recordsWalked() const noexcept { return recordsWalked_; } // terminator included
    Micros durationUs() const noexcept { return durationUs_; }
    const ImportLog& log() const noexcept { return log_; }

    // Decodes samples [first, first + count) of channel `ch` into `out` (count * dtypeSize(outDtype) bytes)
    // with the channel's scaling. Validity is not applied (writeInvalid / clearInvalidBits do that). I32
    // output is accepted only for an identity-scaled integer channel of width <= 4. Fails with Invalid on a
    // bad channel or range and with Io on a page fault inside a mapped file.
    Result<void> decode(std::uint32_t ch, std::uint64_t first, std::uint64_t count, store::Dtype outDtype,
                        std::span<std::byte> out) const;
    // Decode into the channel's dtype, widened to double (tests and tools).
    Result<std::vector<double>> decodeAsDouble(std::uint32_t ch, std::uint64_t first,
                                               std::uint64_t count) const;
    // Stored values without scaling, as double (exact for every integer up to 2^53).
    Result<std::vector<double>> rawValues(std::uint32_t ch, std::uint64_t first, std::uint64_t count) const;

    enum class RangeValidity : std::uint8_t { AllValid, Mixed, AllInvalid };
    RangeValidity validity(std::uint32_t ch, std::uint64_t first, std::uint64_t count) const noexcept;
    // Clears bit (i - first) of `words` (LSB first, 1 = valid) for every invalid sample i of the range.
    void clearInvalidBits(std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                          std::span<std::uint64_t> words) const noexcept;
    // The same over a byte bitmap (bit (i - first) % 8 of byte (i - first) / 8, LSB first; IReader blocks).
    // Both visit only the intervals that touch the range (binary search), not every interval of the channel.
    void clearInvalidBytes(std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                           std::span<std::uint8_t> bytes) const noexcept;
    // Writes NaN at every invalid sample of a decoded F32/F64 range (05 section 1.3 rule 7).
    void writeInvalidNaN(std::uint32_t ch, std::uint64_t first, std::uint64_t count, store::Dtype dtype,
                         std::span<std::byte> out) const noexcept;
    // Is sample i valid according to the gap blocks (stored NaN is not considered here)?
    bool validAt(std::uint32_t ch, std::uint64_t i) const noexcept;

private:
    friend class LdParser;

    std::shared_ptr<const MappedFile> mapped_;
    std::shared_ptr<const std::vector<std::byte>> buffer_;
    std::span<const std::byte> bytes_;
    std::filesystem::path path_;
    LdOptions options_;

    LdHeader header_;
    std::vector<std::byte> rawHeader_;
    std::optional<LdEventRecord> event_;
    std::optional<LdVenueRecord> venue_;
    std::optional<LdVehicleRecord> vehicle_;
    std::optional<LdWeatherRecord> weather_;
    std::vector<std::byte> rawEvent_, rawVenue_, rawVehicle_, rawWeather_;
    std::vector<LdDeviceEvent> events_;
    std::vector<LdEnumTable> enums_;
    std::unordered_map<std::uint16_t, std::size_t> enumByIndex_; // first table of each index
    std::vector<LdChannel> channels_;
    std::uint32_t dialect_ = 0;
    std::string stringEncoding_ = "ascii";
    bool headerOnly_ = false;
    std::uint32_t recordsWalked_ = 0;
    Micros durationUs_ = 0;
    ImportLog log_;
};

} // namespace esses::formats::ld
