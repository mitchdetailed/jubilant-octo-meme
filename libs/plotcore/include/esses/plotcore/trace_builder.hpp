// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TraceBuilder: exact M4 decimation per device-pixel column (04 sections 4.2-4.9, D-012; property test of
// 04 section 4.9: decimate on the chunk path equals a brute-force per-column reference bit-for-bit on
// lo/hi/first/last/flags for F32 columns, encloses it for F64/I32 pyramid buckets).
//
// Column spans (04 section 4.3, exact integer form). x_c = visible.a + c * visible.length() / W and
// t_c = map.timeAt(x_c) (made non-decreasing); column c holds the samples [i_c, i_{c+1}) where i_c is the
// first sample whose time is >= t_c: for a uniform series the smallest i with rate.timeAt(i, t0) >= t_c
// (D-129 arithmetic, the integer form of ceil(sampleAt(x_c))), for a timestamped series
// lower_bound(timesUs, t_c) (one binary search, then a monotone cursor, 04 section 4.8). n_c = i_{c+1} - i_c.
//   regime A (n_c < 2)    raw samples at their exact x (M4Flag::RawRegime, M4Column::xFrac)
//   regime B (2..31)      raw M4 scan
//   regime C (>= 32)      pyramid M4: store::chunkRangeMinMax from level descentLevel(n_c, 8) with the
//                         partial-bucket descent (<= 32 + 54 + 2 reads per column)
// first/last are the first and last VALID samples of the column (raw reads; bitmap scans find them).
// decimate() pins progressively: one chunk at a time through Column::require (a column straddling chunks is
// folded per chunk), Column::prefetch of the next chunk, the CancelToken checked every 256 columns and at
// every chunk transition. In distance mode the samples of laps without a distance index are excluded (their
// laps collapse onto one distance, 04 section 3.1) and count as invalid for line breaking.
//
// Validity and breaks (04 section 4.6): a column without a valid sample is Empty; Empty | HasInvalid breaks
// the polyline, a bare Empty (no samples at all: zoomed in beyond the sample rate, or outside the data) is
// joined across. In regimes A/B an invalid sample breaks the line at the column edge it touches
// (BreakBefore / BreakAfter); an invalid run strictly inside a regime-B column and partial invalidity in a
// regime-C column do not break it (the gap is sub-pixel); HasInvalid is set whenever an invalid sample was
// seen. emit() writes vertices (c, first), (c+0.5, lo/hi), (c+1, last) for B/C columns and one vertex at
// (c + xFrac) for A columns, with Y clamped to the panel +- 2 line widths; Interp::Previous traces step
// horizontally into each raw vertex; style Points (and point markers on a Line trace zoomed in below 0.25
// samples per pixel) emit a Points command, regime C a vertical Lines segment per column.
//
// Deviations from 04 section 4.2 (M1):
//   * TraceSpec holds store::Column handles by value where 04 writes `const Column*`, so a spec copied to a
//     worker cannot dangle (01 section 5.3 rule 9); TraceSpec gains `dash` (overlayStyle, D-100).
//   * M4Flag scopes the flag constants and gains BreakBefore/BreakAfter; M4Column gains xFrac (the exact
//     sub-pixel x of a regime-A sample, which 04 section 4.4 places at map.xOfSample(i)).
//   * Clipped is never set: emit() works on const columns (the clamp itself is applied).
//   * decimateMany() is the "parallel per trace" entry point of 04 section 6.3 item 2.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/store/column.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::plot {

struct M4Column { // one per device-pixel column; display units RELATIVE TO the trace origin
    float first = 0.0F;
    float last = 0.0F;
    float lo = 0.0F;
    float hi = 0.0F;
    std::uint8_t flags = 0; // OR of M4Flag values
    float xFrac = 0.0F;     // RawRegime: the sample's x offset inside the column, [0, 1)
    friend bool operator==(const M4Column&, const M4Column&) = default;
};

struct M4Flag { // M4Column::flags bits
    enum : std::uint8_t {
        Empty = 1,        // no valid sample in the column
        HasInvalid = 2,   // an invalid (or excluded) sample was seen in the column
        Clipped = 4,      // reserved (see the deviations above)
        RawRegime = 8,    // regime A: one sample at xFrac
        Approx = 16,      // summary path: an edge could not be refined (04 section 4.8)
        BreakBefore = 32, // regimes A/B: an invalid sample precedes `first` inside the column
        BreakAfter = 64   // regimes A/B: an invalid sample follows `last` inside the column
    };
};

enum class TraceStyle : std::uint8_t { Line, Points };

struct TraceSpec {
    store::Column column; // the values
    Rate rate;            // uniform series; invalid for a timestamped series
    Micros t0 = 0;
    store::Interp interp = store::Interp::Linear;
    store::SampleKind kind = store::SampleKind::Real;
    store::Column timesUs;      // valid() for a TimestampedSeries (04 section 4.8)
    units::Converter toDisplay; // y = a * base + b, applied while writing (D-010)
    TraceStyle style = TraceStyle::Line;
    Rgba colour = 0x000000FF;
    float widthLogicalPx = 1.0F;
    DashId dash = DashId::Solid;
};

struct YMap { // computed in double
    float yTopPx = 0.0F;
    float yBottomPx = 0.0F;
    double vMin = 0.0;
    double vMax = 1.0;
    float yOf(double v) const noexcept;
};

// One trace of decimateMany: the spec and its slot's mapper.
struct TraceJob {
    TraceSpec spec;
    DomainMapper map;
};

struct DecimatedTrace {
    double origin = 0.0;
    std::vector<M4Column> columns; // widthPx entries
};

class TraceBuilder {
public:
    // Fills exactly widthPx columns (out.size() >= widthPx) for the visible span through `map`. Returns the
    // trace origin: the display value of the first valid sample read (0 if none); columns hold float offsets
    // from it so F64 channels keep sub-pixel precision. Worker only (Column::require). In distance mode a
    // mapper without distance gives all-Empty columns.
    static Result<double> decimate(const TraceSpec& spec, const DomainMapper& map, XSpan visible, int widthPx,
                                   std::span<M4Column> out, const CancelToken& cancel);
    // Summary path (04 section 4.8): SummaryPyramid levels only, never blocks (tryPin for edge refinement);
    // columns that could not be refined are flagged Approx: their lo/hi enclose the exact values and every
    // extra sample lies within one 256-sample bucket of the column boundary.
    static Result<double> decimateSummary(const TraceSpec& spec, const DomainMapper& map, XSpan visible,
                                          int widthPx, std::span<M4Column> out, const CancelToken& cancel);
    // Decimates every job (04 section 6.3 item 2: parallel per trace). With a pool the jobs after the first
    // run as unkeyed Priority::Visible tasks and the calling thread runs the first one and then waits (Rule W
    // runs a still-queued job inline), so it must not be a NoBlock thread; without a pool they run in order.
    // Results are in job order and identical either way.
    static std::vector<Result<DecimatedTrace>> decimateMany(std::span<const TraceJob> jobs, XSpan visible,
                                                            int widthPx, const CancelToken& cancel,
                                                            TaskPool* pool = nullptr);
    // y = yMap.yOf(origin + double(col.v)); x = xOriginPx + c (device px); line width widthLogicalPx * dpr.
    static void emit(DrawList& list, std::span<const M4Column> columns, double origin, const TraceSpec& spec,
                     const YMap& yMap, float xOriginPx, float dpr);
};

} // namespace esses::plot
