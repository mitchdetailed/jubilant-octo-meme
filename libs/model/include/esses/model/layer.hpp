// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataLayer (02 section 4.2): one time base of a source ("Normal", "Fastest", "Burst0") with its channels,
// markers, ranges and enumeration tables.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esses::model {

struct DataLayer {
    std::string name{kNormalLayer};   // "Normal" canonical; "Fastest", "Burst0", ...
    std::optional<UtcInstant> origin; // absolute time of Micros 0
    Micros duration = 0;
    std::vector<Channel> channels; // insertion order = file order; maths/alias/detail appended
    std::unordered_map<std::string, std::uint32_t> byNameCI;       // caseFold(name) -> index
    std::unordered_map<std::string, std::uint32_t> byLoggedNameCI; // caseFold(loggedName) -> index
    std::unordered_map<ChannelId, std::uint32_t> byId;
    std::vector<Marker> markers;     // all classes, all provenances, model order, sidecar edits applied
    std::vector<Marker> baseMarkers; // file merged with companion, before the sidecar: the input every
                                     // sidecar re-application starts from (addition to 02 section 4.2)
    RangeGroupTree ranges;           // output of the rules; one version per group path (D-140)
    std::vector<EnumTable> enumTables;

    // Case-folded lookup on the canonical name only (the resolver does the rest).
    const Channel* find(std::string_view channelName) const;
    const Channel* findLogged(std::string_view loggedName) const;
    const Channel* findById(const ChannelId& id) const;
    Channel* findMutable(std::string_view channelName);
    const EnumTable* enumTable(EnumTableId id) const noexcept;
    const Marker* findMarker(const MarkerId& id) const noexcept;

    // Appends a channel and indexes it; returns its index. On a case-folded name (or logged-name, or id)
    // collision the earlier channel keeps the index entry (importers suffix duplicates, 05 section 3.4).
    std::uint32_t addChannel(Channel channel);
    // Rebuilds byNameCI, byLoggedNameCI and byId from `channels` (after renames or removals).
    void rebuildIndex();
};

} // namespace esses::model
