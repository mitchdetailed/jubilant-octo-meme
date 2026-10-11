// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProbeResult and probe_json (05 section 1.2, schema probe-1): the complete metadata of a log without any
// bulk sample decode. One schema, three uses: the Open dialog's Details pane, the fixture expectation
// expected.probe.json and the v1.0 catalogue row.
//
// Canonical form (02 section 6.5 rule 1): "$schema" and "schemaVersion" first, every other key sorted by
// UTF-8 code units, two-space indent, LF, shortest round-trip numbers, Micros as integers, no non-finite
// number. toJson() returns a model::OpaqueJson (the stand-in for JsonDoc, which esses_base does not have
// yet), so the serialisation is esses_model's one canonical writer; canonicalJson() is the document text.
//
// Members follow probe-1: optional members are omitted when absent (null is never written), a text detail is
// written in the short form (a bare string), a numeric detail as {"t":"number","v","unit"?,"dps"} and a
// date-time as {"t":"datetime","v"}; an Import Log entry's ctx omits absent members and repeats == 1.
#pragma once

#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/store/types.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace esses::formats {

// ChannelInfo::flags bits (05 section 1.2 ChannelFlags).
namespace channel_flags {
inline constexpr std::uint32_t kVendorDerived = 1u << 0;
inline constexpr std::uint32_t kHasDisplayRange = 1u << 1;
inline constexpr std::uint32_t kStatusBits = 1u << 2;
inline constexpr std::uint32_t kUnitFromShortName = 1u << 3;
inline constexpr std::uint32_t kDtypeGuessed = 1u << 4;
} // namespace channel_flags

// A channel whose sample times come from IReader::readTimestamps.
struct Timestamped {
    friend bool operator==(const Timestamped&, const Timestamped&) = default;
};

struct ChannelInfo {
    std::uint32_t index = 0;                       // position in the layer, stable for the life of the reader
    std::string loggedName, shortName, vendorUnit; // raw strings as found (trimmed, NUL-stripped, UTF-8)
    std::optional<std::string> vendorId;           // .ld: the record counter
    std::string vendorDtype;                       // on-disk type: "i16", "u32", "f16", "f64", ...
    store::Dtype dtype = store::Dtype::F32;        // storage dtype after decode (F32 | F64 | I32)
    store::SampleKind kind = store::SampleKind::Real;
    std::variant<Rate, Timestamped> timing = Rate{};
    std::uint64_t sampleCount = 0;
    Micros t0Us = 0;
    int dps = 0;
    std::optional<std::uint32_t> enumTable;
    std::uint32_t flags = 0; // channel_flags bits

    friend bool operator==(const ChannelInfo&, const ChannelInfo&) = default;
};

// Markers including device events (cls DeviceEvent, group "DeviceEvents", flags = event code; 02
// section 4.3).
struct MarkerInfo {
    model::MarkerClass cls = model::MarkerClass::SOL;
    Micros tUs = 0;
    std::string name, group, comment;
    std::uint32_t flags = 0;
    std::uint32_t ordinal = 0; // index among markers of the same (class, group, tUs) in file order

    friend bool operator==(const MarkerInfo&, const MarkerInfo&) = default;
};

struct LayerInfo {
    std::string name = "Normal";
    Micros durationUs = 0;
    std::optional<UtcInstant> utc; // absolute time of Micros 0
    bool utcAssumedLocal = false;  // the file states a local time without a zone (05 section 3.7)
    std::vector<ChannelInfo> channels;
    std::vector<MarkerInfo> markers; // the log's and its companion's, ordered by (tUs, stored order)

    friend bool operator==(const LayerInfo&, const LayerInfo&) = default;
};

struct EnumTableInfo {
    std::uint32_t id = 0;
    std::vector<model::EnumEntry> entries; // {value, label, severity 0 normal ... 3 fault}

    friend bool operator==(const EnumTableInfo&, const EnumTableInfo&) = default;
};

struct ProbeResult {
    static constexpr std::string_view kSchemaId = "urn:esses:probe-1";
    static constexpr int kSchemaVersion = 1;

    std::string importerId, importerVersion;
    std::string dialect; // comma-separated dialect flags in the importer's order, "" when none
    // As found: the log's own details (prov Logged), then its companion's (prov Companion). Provenance is not
    // serialised; fromJson() returns every detail as Logged.
    std::vector<model::Detail> details;
    std::vector<LayerInfo> layers; // layers[0].name == "Normal"
    std::vector<EnumTableInfo> enumTables;
    std::vector<std::filesystem::path> companions; // file names only (relative to the source's directory)
    std::vector<ImportLogEntry> warnings;          // sniff/probe/open entries only (05 section 1.1)

    // probe_json. Errors: Invalid when a number is not finite (context.detail = JSON pointer).
    Result<model::OpaqueJson> toJson() const;
    // The canonical document text (trailing LF) of toJson().
    Result<std::string> canonicalJson() const;
    // Reads probe-1 (any member order, "$schema" in the urn:esses:, esses: or https form). Errors: Invalid
    // with the JSON pointer in context.detail (missing or mistyped member, unknown member), Unsupported for
    // another schema major. Provenance of details comes back as Logged.
    static Result<ProbeResult> fromJson(const model::OpaqueJson& json);
    static Result<ProbeResult> fromJsonText(std::string_view json);

    friend bool operator==(const ProbeResult&, const ProbeResult&) = default;
};

// Stable spellings used by probe_json ("F32", "Real", ...).
std::optional<store::Dtype> parseDtype(std::string_view text) noexcept;
std::optional<store::SampleKind> parseSampleKind(std::string_view text) noexcept;
std::optional<ImportLogEntry::Level> parseImportLogLevel(std::string_view text) noexcept;

} // namespace esses::formats
