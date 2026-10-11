// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Channel identity and descriptor (02 section 4.1; D-013, D-060, D-063, D-130). The name is the identity
// users and documents use; ChannelId is a deterministic UUID that differs between two files logging the same
// channel and is unaffected by display overrides.
#pragma once

#include <esses/base/rate.hpp>
#include <esses/model/ids.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

using esses::Micros;
using esses::Rate;

// Declared in esses_store (Resampler::defaultFor needs it below this layer) and re-exported here (02 4.1).
using SampleKind = store::SampleKind;

// Where a channel comes from (D-013, D-063). The lower-case name is the first part of the ChannelId hash.
enum class ChannelSource : std::uint8_t { Logged, Maths, Detail, Alias, Mapped, Constant, Setup };
std::string_view channelSourceName(ChannelSource s) noexcept; // "logged", "maths", "detail", ...
std::optional<ChannelSource> parseChannelSource(std::string_view text) noexcept;

struct EnumEntry {
    std::int32_t value = 0;
    std::string label;
    std::uint8_t severity =
        0; // 0 normal, 1 info, 2 warning, 3 fault (.ld enumeration blocks, 05 section 3.9)

    friend bool operator==(const EnumEntry&, const EnumEntry&) = default;
};

struct EnumTable {
    EnumTableId id;
    std::string name;
    std::vector<EnumEntry> entries;

    const EnumEntry* find(std::int32_t value) const noexcept; // first entry with that value
    friend bool operator==(const EnumTable&, const EnumTable&) = default;
};

// Opaque on-disk encoding of one channel, owned by the importer that read it (05 section 1.3
// IReader::sourceEncoding; .ld: LdChannelEncoding). A writer uses it only when importerId() matches its own
// format (05 section 5 item 2). Never part of ChannelId, project documents or the sidecar.
class SourceEncoding {
public:
    virtual ~SourceEncoding() = default;
    virtual std::string_view importerId() const = 0;

protected:
    SourceEncoding() = default;
    SourceEncoding(const SourceEncoding&) = default;
    SourceEncoding& operator=(const SourceEncoding&) = default;
    SourceEncoding(SourceEncoding&&) = default;
    SourceEncoding& operator=(SourceEncoding&&) = default;
};

struct ChannelDescriptor { // D-013 field set plus sourceEncoding (02 section 4.1, amended 2026-10-10)
    ChannelId id;
    std::string name;       // canonical analysis name: the lookup key for workbooks, maths, overrides
    std::string shortName;  // <= 8 characters for gauges; empty = derived from name (displayShortName())
    std::string loggedName; // as spelled by the file; equals name unless a dictionary or mapping renamed it
    units::UnitId unit = units::kNoUnit; // storage (base) unit of the quantity
    units::QuantityId quantity = units::kUnitless;
    store::Dtype dtype = store::Dtype::F32;
    SampleKind kind = SampleKind::Real;
    store::Interp interp = store::Interp::Linear;
    int dps = 2;          // display decimals from the file (.ld: byte 0x64 of the record); overrides win
    int colourIndex = -1; // -1 = automatic from the scheme palette
    std::optional<EnumTableId> enums;
    ChannelSource source = ChannelSource::Logged;
    std::string statusText;   // "", "Pending", "Error: ..."; mirrored by Channel::status (D-060)
    std::optional<Rate> rate; // nullopt for a timestamped series
    std::shared_ptr<const SourceEncoding> sourceEncoding; // null unless the importer kept one; shared on copy

    // shortName when set, else the first eight code points of name.
    std::string displayShortName() const;
    // Value equality of every member; sourceEncoding compares by pointer.
    friend bool operator==(const ChannelDescriptor&, const ChannelDescriptor&) = default;
};

enum class ChannelStatus : std::uint8_t { Ok, Pending, Error };
std::string_view channelStatusName(ChannelStatus s) noexcept; // "Ok", "Pending", "Error"
// The status a statusText stands for: "" -> Ok, "Pending" -> Pending, "Error..." -> Error, anything else Ok.
ChannelStatus statusFromText(std::string_view statusText) noexcept;

struct Channel {
    ChannelDescriptor desc;
    store::Series series;
    ChannelStatus status = ChannelStatus::Ok;

    // Sets status and desc.statusText together ("" for Ok, "Pending", "Error: <message>" for Error).
    void setStatus(ChannelStatus s, std::string_view message = {});
};

// ---- ChannelId derivation (02 section 4.1 table): uuidFromHash(hash128({parts...})) ----------------------
// The SourceId part is its 32-digit hex form; the layer part goes through canonicalLayerName ("" -> Normal).
// Logged: "logged", SourceId hex, layer, logged name, importer id.
ChannelId channelIdLogged(const SourceId& source, std::string_view layer, std::string_view loggedName,
                          std::string_view importerId);
// Maths, global, built-in and transient modules: "maths", SourceId hex, layer, canonical name,
// "module:" + module id (moduleId without the prefix, e.g. "esses.corrected_speed_distance", "~transient").
ChannelId channelIdMathsModule(const SourceId& source, std::string_view layer, std::string_view name,
                               std::string_view moduleId);
// Maths, local (sidecar) module: "maths", SourceId hex, layer, canonical name, "local".
ChannelId channelIdMathsLocal(const SourceId& source, std::string_view layer, std::string_view name);
// Alias, Mapped, Detail, Constant, Setup: the source kind in lower case, SourceId hex, layer, canonical
// name, "". Logged and Maths need their own functions above (ESSES_ASSERT; release builds hash "" as well).
ChannelId channelIdOther(ChannelSource kind, const SourceId& source, std::string_view layer,
                         std::string_view name);

} // namespace esses::model
