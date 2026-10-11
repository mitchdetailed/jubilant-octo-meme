// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The hierarchical boundary descent shared by the chunk pyramid and the summary pyramid (D-012, 04
// section 4.5). Units are "leaves" (raw samples for a chunk, 256-sample buckets for the summary); level k has
// buckets of 4^(k+1) leaves and the last bucket of a level may be partial. Over the half-open leaf span [p,
// q):
//   whole buckets [j0, j1) of the current level are folded directly;
//   the left remainder [p, j0*B) (aligned at its right end) and the right remainder [j1*B, q) (aligned at its
//   left end) are refined one level down, so each edge costs at most 3 bucket reads per level;
//   below level 0 the remaining leaves are folded one by one.
// When q is the leaf count, the partial last bucket counts as whole (it covers exactly [j*B, q)).
#pragma once

#include <esses/store/types.hpp>

#include <algorithm>
#include <cstdint>

namespace esses::store::detail {

// leaf(p, q): fold leaves [p, q). bucket(level, j0, j1): fold buckets [j0, j1) of `level`.
template <class LeafFold, class BucketFold>
void descend(std::uint64_t p, std::uint64_t q, int level, std::uint64_t leafCount, LeafFold& leaf,
             BucketFold& bucket) {
    while (p < q) {
        if (level < 0) {
            leaf(p, q);
            return;
        }
        const std::uint64_t b = bucketSize(static_cast<unsigned>(level));
        const std::uint64_t j0 = (p + b - 1) / b;
        const std::uint64_t j1 = q == leafCount ? (q + b - 1) / b : q / b;
        if (j0 < j1) {
            bucket(static_cast<unsigned>(level), j0, j1);
            descend(p, std::min(j0 * b, q), level - 1, leafCount, leaf, bucket);
            p = std::min(j1 * b, q);
        }
        --level;
    }
}

} // namespace esses::store::detail
