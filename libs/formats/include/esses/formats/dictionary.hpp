// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dictionary (05 section 8.3, 02 section 4.6 rule 15; schema dictionary-1,
// data/dictionaries/<importerStem>.json): per-importer channel knowledge applied by the open pipeline before
// the layer is built (05 section 2 step 4a). Entries match by vendorId, exact name or nameRegex (first match
// wins, in file order) and overwrite the channel's name, unit, quantity, interpolation, kind and dtype;
// bitfields expand a group channel into derived Bool channels (name = (group & mask) != 0, source Mapped)
// sharing the group's Column through a BitProvider, filtered by the Device Type / Device Version details.
// `version` is the dictionary's content version and enters the column cache key.
#pragma once

#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::formats {

class Dictionary {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:dictionary-1";

    struct Match {
        std::optional<std::string> vendorId; // a string, as the file writes it
        std::optional<std::string> name;
        std::optional<std::string> nameRegex;
    };
    struct Entry {
        Match match;
        std::optional<std::string> name, shortName, unit, quantity, kind, interp, dtype;
        model::OpaqueJson lapSource; // {"kind": "beacon_event"} etc.; null when absent
    };
    struct Bit {
        std::uint64_t mask = 0; // written as "0x0001"
        std::string name;
    };
    struct Device {
        std::string type;
        std::optional<int> minVersion;
        std::optional<int> maxVersion;
    };
    struct Bitfield {
        Match match;
        std::optional<Device> device;
        std::vector<Bit> bits;
    };

    std::string importer; // importer id ("motec.ld", "text.profile:generic.csv")
    std::string version;  // content version
    std::vector<Entry> entries;
    std::vector<Bitfield> bitfields;
    model::OpaqueJson unknown; // unrecognised top-level members, preserved

    static Result<Dictionary> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Dictionary> load(const std::filesystem::path& path, std::vector<std::string>& log);
    Result<std::string> toJson() const;

    // The first entry matching a channel (vendorId, then exact name, then nameRegex, each in file order), or
    // nullptr.
    const Entry* match(std::string_view vendorId, std::string_view name) const;
};

} // namespace esses::formats
