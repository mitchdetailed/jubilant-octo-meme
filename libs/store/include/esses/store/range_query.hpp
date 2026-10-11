// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Column-level range queries over pinned chunks: the store side of exact M4 (04 sections 4.5-4.6, D-012).
// TraceBuilder (esses_plotcore, M1) computes the per-pixel-column span [i0, i1), picks the start level with
// descentLevel(n, 8) and calls rangeMinMax on the chunks it holds pinned; a span that crosses a chunk
// boundary is folded per chunk (65,536 is a multiple of every bucket size, so no bucket crosses a chunk).
#pragma once

#include <esses/store/chunk_cache.hpp>
#include <esses/store/pyramid.hpp>
#include <esses/store/types.hpp>

#include <optional>

namespace esses::store {

// Min/max over the half-open column span [begin, end) (clamped at 0 below; samples past the column end are
// ignored) by hierarchical descent from `topLevel` (kAutoLevel, kRawOnly or an explicit level) in each chunk.
// nullopt when a chunk the span needs is not pinned by `pinned`. Exact for F32; the directionally rounded
// exact result for F64 and I32 (pyramid.hpp).
std::optional<MinMax> rangeMinMax(const ChunkHandle& pinned, SampleIdx begin, SampleIdx end,
                                  int topLevel = kAutoLevel, DescentStats* stats = nullptr) noexcept;

// Whether [begin, end) holds an invalid sample (M4's HasInvalid flag in regime C, 04 section 4.6): a mode
// branch per chunk plus a word scan of the bitmap, which is authoritative after seal. nullopt when a needed
// chunk is not pinned.
std::optional<bool> rangeHasInvalid(const ChunkHandle& pinned, SampleIdx begin, SampleIdx end) noexcept;

} // namespace esses::store
