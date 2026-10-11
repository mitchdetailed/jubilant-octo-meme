// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Identity types of the data-source model (02 section 4.1, 4.3, 4.4; D-013, D-014, D-130).
//
// SourceId is the quick content hash of a log (xxh3-128 over the size, the first and last MiB and the format
// header; D-130). ChannelId, MarkerId and RangeId are deterministic UUIDs derived through hash128(parts)
// (02 section 1 behaviour 3) by the functions in channel.hpp, marker.hpp and range.hpp; user-created markers
// and annotations get uuidRandom(). EnumTableId is the importer's table number.
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/uuid.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace esses::model {

using SourceId = esses::Hash128; // quick content hash (05 section 1.1); the full hash lives in provenance

struct ChannelId {
    Uuid v;
    std::string str() const { return v.str(); }
    friend auto operator<=>(const ChannelId&, const ChannelId&) = default;
    friend bool operator==(const ChannelId&, const ChannelId&) = default;
};

struct MarkerId {
    Uuid v;
    std::string str() const { return v.str(); }
    bool isNil() const noexcept { return v.isNil(); }
    friend auto operator<=>(const MarkerId&, const MarkerId&) = default;
    friend bool operator==(const MarkerId&, const MarkerId&) = default;
};

struct RangeId {
    Uuid v;
    std::string str() const { return v.str(); }
    friend auto operator<=>(const RangeId&, const RangeId&) = default;
    friend bool operator==(const RangeId&, const RangeId&) = default;
};

struct EnumTableId {
    std::uint32_t v = 0;
    friend constexpr auto operator<=>(const EnumTableId&, const EnumTableId&) = default;
    friend constexpr bool operator==(const EnumTableId&, const EnumTableId&) = default;
};

// The canonical name of the main data layer. "" is accepted as its alias on import (D-015) and is mapped to
// "Normal" by every lookup and every id derivation in this library.
inline constexpr std::string_view kNormalLayer = "Normal";
constexpr std::string_view canonicalLayerName(std::string_view layer) noexcept {
    return layer.empty() ? kNormalLayer : layer;
}

namespace detail {
// The 16 bytes folded into two u64s (02 section 4.1's std::hash<ChannelId>).
inline std::size_t foldUuid(const Uuid& u) noexcept {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::memcpy(&lo, u.b.data(), 8);
    std::memcpy(&hi, u.b.data() + 8, 8);
    return static_cast<std::size_t>(lo ^ (hi * 0x9E3779B97F4A7C15ull));
}
} // namespace detail

} // namespace esses::model

template <> struct std::hash<esses::model::ChannelId> {
    std::size_t operator()(const esses::model::ChannelId& id) const noexcept {
        return esses::model::detail::foldUuid(id.v);
    }
};
template <> struct std::hash<esses::model::MarkerId> {
    std::size_t operator()(const esses::model::MarkerId& id) const noexcept {
        return esses::model::detail::foldUuid(id.v);
    }
};
template <> struct std::hash<esses::model::RangeId> {
    std::size_t operator()(const esses::model::RangeId& id) const noexcept {
        return esses::model::detail::foldUuid(id.v);
    }
};
template <> struct std::hash<esses::model::EnumTableId> {
    std::size_t operator()(const esses::model::EnumTableId& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.v);
    }
};
