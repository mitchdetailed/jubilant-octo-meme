// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Keys and handles of the maths graph (03 section 4.3 rule 9, 03 section 7.1-7.2, 01 section 5.4).
//   ChannelKey   { SourceId src; layer; nameCf }: the public key; resolves to the chain head (03 section 4.3)
//   Version      xxh3 of the node's definition and exactly what it reads (03 section 7.2, D-140)
//   SampleRange  the demanded sample span of MathGraph::require
//   ColumnHandle what a completed require() delivers: the output's descriptor and Series (a Column on a
//                PlanProvider, sealed into the shared ChunkCache like logged data) and the runtime
//                diagnostics of the evaluation (MathGraph::applyCompletion copies them into MathStatus)
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/ids.hpp>
#include <esses/store/series.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace esses::maths {

struct ChannelKey {
    model::SourceId src;
    std::string layer;  // canonical layer name ("Normal")
    std::string nameCf; // case-folded channel name

    // Canonicalises the layer and case-folds the name (esses::caseFold).
    static ChannelKey make(const model::SourceId& src, std::string_view layer, std::string_view name);
    friend bool operator==(const ChannelKey&, const ChannelKey&) = default;
};

using Version = Hash128;

struct SampleRange { // half-open [first, end) in samples of the output; all() = the whole extent
    SampleIdx first = 0;
    SampleIdx end = std::numeric_limits<SampleIdx>::max();

    static constexpr SampleRange all() noexcept { return {}; }
    friend bool operator==(const SampleRange&, const SampleRange&) = default;
};

struct ColumnHandle {
    ChannelKey key;
    Version version;
    model::ChannelDescriptor descriptor; // rate set; statusText empty (the status is MathGraph's)
    store::Series series;                // a UniformSeries at the result rate (03 section 6.4)
    Diagnostics diags;                   // runtime warnings and infos of the evaluation (W-CSD-FALLBACK)
    Hash128 selectionKey;                // M2: the selection a SelectionDependent output was evaluated for
};

} // namespace esses::maths

template <> struct std::hash<esses::maths::ChannelKey> {
    std::size_t operator()(const esses::maths::ChannelKey& k) const noexcept {
        std::size_t h = std::hash<esses::Hash128>{}(k.src);
        h ^= std::hash<std::string>{}(k.layer) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.nameCf) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }
};
