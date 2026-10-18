// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelOverrides (02 section 6.3, 06 section 13.1, D-010, D-013; schema overrides-1,
// <Project>/channels/overrides.json): per-channel display settings keyed by channel name (display unit, DPS,
// colour index, scale, interpolation, warning/fault limits). The Channel Editor presents the map as rows and
// converts on load and save. Every {v, unit} keeps the unit as written (model::Quantityd::symbol), so a
// canonical document round-trips byte-identically (AC-M2-07); unknown channels and unknown members are kept
// untouched. An override whose displayUnit is of another family than the channel in some open source is
// dropped for the session with a Messages line (D-010) by the host, never by this type.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/detail.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

class ChannelOverrides {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:overrides-1";

    struct Entry {
        std::optional<std::string> displayUnit; // unit symbol as written
        std::optional<int> dps;
        std::optional<int> colourIndex;
        std::optional<bool> autoScale;
        std::optional<model::Quantityd> scaleMin;
        std::optional<model::Quantityd> scaleMax;
        std::optional<std::string> interp; // "Linear" | "Closest" | "Previous" | "Next"
        JsonDoc warn;                      // {"min"?: {v, unit}, "max"?: {v, unit}}; null when absent
        JsonDoc fault;
        JsonDoc unknown; // unrecognised members of the entry, preserved
        bool empty() const noexcept;
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    std::map<std::string, Entry, std::less<>> channels; // key: channel name as written
    int schemaVersion = 1;
    JsonDoc unknown; // unrecognised top-level members, preserved

    static Result<ChannelOverrides> parse(std::string_view json, std::vector<std::string>& log);
    static Result<ChannelOverrides> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text; "" on a non-finite number
    Result<void> save(const std::filesystem::path& path) const;

    // The entry of a channel, matched case-insensitively (D-013), or nullptr.
    const Entry* find(std::string_view channel) const;
    // Sets (an empty entry removes) the override of `channel`; an existing key matched case-insensitively
    // keeps its spelling.
    void set(std::string_view channel, Entry entry);
    bool clear(std::string_view channel);

    friend bool operator==(const ChannelOverrides&, const ChannelOverrides&) = default;
};

} // namespace esses::project
