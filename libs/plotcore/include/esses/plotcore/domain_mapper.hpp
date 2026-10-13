// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DomainMapper and DistanceChain (04 sections 3.1-3.4; D-016, D-099, D-134). One mapper per (trace, slot):
//   Time:      xOf(t) = (t - R.start) + R.offsetUs         timeAt(x) = R.start + x - R.offsetUs
//   Distance:  xOf(t) = chain.distAt(t) + R.offsetM        timeAt(x) = chain.timeAt(x - R.offsetM)
//   sampleAt(x) = (timeAt(x) - t0) * num / (1e6 * den)     (fractional sample index of the trace's series)
// Time arithmetic is exact integer arithmetic (D-008). DistanceChain composes the outing's per-lap distance
// lookups and stretches: piece i has the stretched length l_i = lookup_i.lengthM * stretch_i, and its start
// R_i is accumulated outward from the origin piece m (R_m = 0), so inside the origin lap the chain equals
// that lap's DistanceIndex exactly (times its stretch) and the next lap starts exactly at l_m. With the
// origin o (R.start, or the outing start for Outing ranges) and u_o = lookup_m.distAt(o) * stretch_m (0 for
// a lap range):
//   distAt(t) = R_p + lookup_p.distAt(t) * stretch_p - u_o              (p: the piece containing t)
//   timeAt(d) = p.start + llround(lookup_p.timeAt((d + u_o - R_p) / stretch_p) * 1e6)
// which is 04 section 3.1's D_p - D_m form with the origin allowed inside a lap. A piece without a lookup
// has length 0: its lap collapses onto one distance and draws nothing (TraceBuilder excludes its samples).
// A mapper whose chain has no indexed piece has hasDistance() == false and distance mode draws the "no
// distance data" banner for that slot (AC-M1-09). Mappers hold shared_ptr<const ...> only (01 section 5.3
// rule 9).
//
// Additions to 04 section 3.1 (M1):
//   * ILapDistance is the per-lap lookup the chain composes. LapPiece::idx (analysis::DistanceIndex) is the
//     production source; LapPiece::lookup lets tests and later producers supply known tables (it wins over
//     idx when both are set).
//   * DistanceChain::anyIndexed, indexedAt, coversTime, coversDistance and unindexedSpans;
//     DomainMapper::pxOf / xOfPx.
//   * The F9 rule of 04 section 3.4 / AC-M1-09 as pure functions (mainExtentIn, outingExtentIn, convertZoom),
//     so WorksheetViewModel::setDomain and the worksheet switch of D-133 share one tested implementation.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/time.hpp>
#include <esses/plotcore/view_state.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace esses::plot {

struct RangeGeom { // from model::Range (D-014)
    Micros start = 0;
    Micros end = 0;
    Micros offsetUs = 0;
    double offsetM = 0.0;
    double stretch = 1.0;
};

struct OutingGeom { // the slot source's outing containing R
    Micros start = 0;
    Micros end = 0;
};

// The per-lap lookups of a DistanceIndex (D-016): distAt takes absolute layer time and returns metres since
// the lap start (t clamped to the lap); timeAt returns SECONDS since the lap start of the first time the
// distance d is reached (d clamped to [0, lengthM], D-099). Implementations are immutable and safe for
// concurrent const calls.
class ILapDistance {
public:
    virtual ~ILapDistance() = default;
    virtual double distAt(Micros t) const noexcept = 0;
    virtual double timeAt(double d) const noexcept = 0;
    virtual double lengthM() const noexcept = 0;

protected:
    ILapDistance() = default;
    ILapDistance(const ILapDistance&) = default;
    ILapDistance& operator=(const ILapDistance&) = default;
};

struct LapPiece {
    Micros start = 0;
    Micros end = 0;
    std::shared_ptr<const analysis::DistanceIndex> idx; // null: lap without an index
    double stretch = 1.0;
    std::shared_ptr<const ILapDistance> lookup; // optional; wins over idx (tests, other producers)
};

class DistanceChain {
public:
    // Laps of the outing in time order, contiguous; origin = R.start (or the outing start for Outing ranges).
    DistanceChain(std::vector<LapPiece> pieces, Micros origin);

    bool complete() const noexcept;         // every piece has an index (and there is at least one piece)
    bool anyIndexed() const noexcept;       // at least one piece has an index
    double distAt(Micros t) const noexcept; // metres from the origin; negative before it
    Micros timeAt(double d) const noexcept; // inverse; clamped to the outing
    XSpan extent() const noexcept;          // [dist(outing.start), dist(outing.end)) relative to the origin
    // The piece containing t (t clamped to the outing) has an index.
    bool indexedAt(Micros t) const noexcept;
    // The time span [t0, t1) lies inside the outing and every piece it overlaps has an index.
    bool coversTime(Micros t0, Micros t1) const noexcept;
    // The distance span [a, b) lies inside extent() and crosses no lap without an index.
    bool coversDistance(double a, double b) const noexcept;
    // Time spans [start, end) of the laps without an index, in time order.
    std::vector<std::pair<Micros, Micros>> unindexedSpans() const;
    const std::vector<LapPiece>& pieces() const noexcept { return pieces_; }
    Micros origin() const noexcept { return origin_; }

private:
    std::size_t pieceAtTime(Micros t) const noexcept;
    std::size_t pieceAtDistance(double u) const noexcept; // u: metres from the start of the origin piece

    std::vector<LapPiece> pieces_;
    std::vector<std::shared_ptr<const ILapDistance>> lookups_; // per piece; null without an index
    std::vector<double> length_;                               // l_i (0 without an index)
    std::vector<double> rel_;                                  // R_i, with R_m = 0
    std::size_t originPiece_ = 0;                              // m
    double originLocal_ = 0.0;                                 // u_o
    Micros origin_ = 0;
};

class DomainMapper {
public:
    DomainMapper(Domain domain, RangeGeom range, OutingGeom outing,
                 std::shared_ptr<const DistanceChain> chain, Rate rate, Micros seriesT0);

    Domain domain() const noexcept { return domain_; }
    bool hasDistance() const noexcept;        // false: distance mode draws the banner for this slot
    double xOf(Micros tAbs) const noexcept;   // domain units from R.start, alignment offset applied
    Micros timeAt(double x) const noexcept;   // inverse of xOf
    double sampleAt(double x) const noexcept; // fractional sample index in this trace's series
    double xOfSample(double i) const noexcept;
    double extent() const noexcept;      // |R| in domain units
    XSpan outingExtent() const noexcept; // time: [o.start - R.start, o.end - R.start); distance: chain
    const RangeGeom& range() const noexcept { return range_; }
    const OutingGeom& outing() const noexcept { return outing_; }
    const std::shared_ptr<const DistanceChain>& chain() const noexcept { return chain_; }
    Rate rate() const noexcept { return rate_; }
    Micros seriesT0() const noexcept { return seriesT0_; }

    // Device-pixel position of the domain value x in a visible span drawn over [x0Px, x0Px + widthPx), and
    // its inverse.
    static double pxOf(double x, XSpan visible, double x0Px, double widthPx) noexcept;
    static double xOfPx(double px, XSpan visible, double x0Px, double widthPx) noexcept;

private:
    Domain domain_;
    RangeGeom range_;
    OutingGeom outing_;
    std::shared_ptr<const DistanceChain> chain_;
    Rate rate_;
    Micros seriesT0_;
};

// ---- F9 (04 section 3.4, D-016, AC-M1-09)
// ------------------------------------------------------------------ The Main mapper with alignment offsets
// excluded: every function works on the Main range geometry and its chain (origin R.start).
// `noDistanceLength` is extent'(R) in distance mode without an index: the bound track map's length when the
// Main source has one, else 1.0.

// |R| in `domain` units: time R.end - R.start; distance chain->distAt(R.end) when the chain covers R, else
// noDistanceLength.
double mainExtentIn(Domain domain, const RangeGeom& main, const DistanceChain* chain,
                    double noDistanceLength = 1.0) noexcept;
// outingExtent'(R): time [o.start - R.start, o.end - R.start); distance the chain's extent when the chain
// covers R (collapsed unindexed laps elsewhere in the outing have length 0), else the time extent scaled by
// noDistanceLength / |R| (the fraction rule applied to the outing, in mainExtentIn's units).
XSpan outingExtentIn(Domain domain, const RangeGeom& main, const OutingGeom& outing,
                     const DistanceChain* chain, double noDistanceLength = 1.0) noexcept;

struct ZoomConversion {
    XSpan zoom;
    bool throughChain = false; // false: the zoomFrac rule was used (or from == to)
};

// WorksheetViewModel::setDomain: converts the shared zoom span (measured from R.start) from `from` to `to`.
// Through the chain when it covers the span (Time->Distance a' = chain.distAt(R.start + a); Distance->Time
// a' = chain.timeAt(a) - R.start), else by the zoomFrac rule a' = fa * extent'(R), b' = fb * extent'(R);
// then clamped to outingExtent'(R) (the whole outing extent when the clamp leaves nothing). from == to
// returns the span unchanged.
ZoomConversion convertZoom(XSpan zoom, Domain from, Domain to, double fa, double fb, const RangeGeom& main,
                           const OutingGeom& outing, const DistanceChain* chain,
                           double noDistanceLength = 1.0) noexcept;

} // namespace esses::plot
