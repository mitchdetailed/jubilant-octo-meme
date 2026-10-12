// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/domain_mapper.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace esses::plot {

namespace {

// analysis::DistanceIndex as an ILapDistance.
class IndexLookup final : public ILapDistance {
public:
    explicit IndexLookup(std::shared_ptr<const analysis::DistanceIndex> idx) : idx_(std::move(idx)) {}
    double distAt(Micros t) const noexcept override { return idx_->distAt(t); }
    double timeAt(double d) const noexcept override { return idx_->timeAt(d); }
    double lengthM() const noexcept override { return idx_->lengthM(); }

private:
    std::shared_ptr<const analysis::DistanceIndex> idx_;
};

Micros secondsToMicros(double seconds) noexcept {
    if (!std::isfinite(seconds)) {
        return 0;
    }
    return static_cast<Micros>(std::llround(seconds * 1e6));
}

} // namespace

// ---- DistanceChain
// -------------------------------------------------------------------------------------------

DistanceChain::DistanceChain(std::vector<LapPiece> pieces, Micros origin)
    : pieces_(std::move(pieces)), origin_(origin) {
    const std::size_t n = pieces_.size();
    lookups_.resize(n);
    length_.assign(n, 0.0);
    rel_.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const LapPiece& p = pieces_[i];
        if (p.lookup) {
            lookups_[i] = p.lookup;
        } else if (p.idx) {
            lookups_[i] = std::make_shared<const IndexLookup>(p.idx);
        }
        if (lookups_[i]) {
            const double stretch = p.stretch > 0.0 && std::isfinite(p.stretch) ? p.stretch : 1.0;
            pieces_[i].stretch = stretch;
            length_[i] = lookups_[i]->lengthM() * stretch;
        }
    }
    if (n == 0) {
        return;
    }
    originPiece_ = pieceAtTime(origin_);
    // R_i accumulated outward from the origin piece, so R_m = 0 and R_{m+1} = l_m exactly.
    for (std::size_t i = originPiece_ + 1; i < n; ++i) {
        rel_[i] = rel_[i - 1] + length_[i - 1];
    }
    for (std::size_t i = originPiece_; i-- > 0;) {
        rel_[i] = rel_[i + 1] - length_[i];
    }
    if (const auto& lookup = lookups_[originPiece_]) {
        originLocal_ = lookup->distAt(origin_) * pieces_[originPiece_].stretch;
    }
}

std::size_t DistanceChain::pieceAtTime(Micros t) const noexcept {
    // The last piece whose start is <= t; piece 0 before the outing, the last piece after it.
    const auto it = std::upper_bound(pieces_.begin(), pieces_.end(), t,
                                     [](Micros value, const LapPiece& p) { return value < p.start; });
    if (it == pieces_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(pieces_.begin(), it)) - 1;
}

std::size_t DistanceChain::pieceAtDistance(double u) const noexcept {
    // The last piece with R_p <= u: among pieces sharing one start (laps without an index have length 0) that
    // is the one after them, so a distance at a collapsed lap's position falls into the next indexed lap.
    const auto it = std::upper_bound(rel_.begin(), rel_.end(), u);
    if (it == rel_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(rel_.begin(), it)) - 1;
}

bool DistanceChain::complete() const noexcept {
    if (pieces_.empty()) {
        return false;
    }
    return std::all_of(lookups_.begin(), lookups_.end(), [](const auto& l) { return l != nullptr; });
}

bool DistanceChain::anyIndexed() const noexcept {
    return std::any_of(lookups_.begin(), lookups_.end(), [](const auto& l) { return l != nullptr; });
}

double DistanceChain::distAt(Micros t) const noexcept {
    if (pieces_.empty()) {
        return 0.0;
    }
    const std::size_t p = pieceAtTime(t);
    const auto& lookup = lookups_[p];
    if (!lookup) {
        return rel_[p] - originLocal_; // a collapsed lap: its start position
    }
    const double local = lookup->distAt(t) * pieces_[p].stretch;
    if (p == originPiece_) {
        return local - originLocal_; // exact inside the origin lap (originLocal_ is 0 for a lap range)
    }
    return (rel_[p] - originLocal_) + local;
}

Micros DistanceChain::timeAt(double d) const noexcept {
    if (pieces_.empty()) {
        return origin_;
    }
    if (!std::isfinite(d)) {
        return d > 0.0 ? pieces_.back().end : pieces_.front().start;
    }
    const double u = d + originLocal_; // metres from the start of the origin piece
    if (u < rel_.front()) {
        return pieces_.front().start;
    }
    const std::size_t last = pieces_.size() - 1;
    if (u >= rel_[last] + length_[last]) {
        return pieces_.back().end;
    }
    const std::size_t p = pieceAtDistance(u);
    const auto& lookup = lookups_[p];
    if (!lookup) {
        return pieces_[p].end; // only reachable past the last indexed lap
    }
    const double local = p == originPiece_ ? u : u - rel_[p];
    const double seconds = lookup->timeAt(local / pieces_[p].stretch);
    const Micros t = pieces_[p].start + secondsToMicros(seconds);
    return std::clamp(t, pieces_[p].start, std::max(pieces_[p].start, pieces_[p].end));
}

XSpan DistanceChain::extent() const noexcept {
    if (pieces_.empty()) {
        return {};
    }
    const std::size_t last = pieces_.size() - 1;
    const double a = originPiece_ == 0 ? -originLocal_ : rel_.front() - originLocal_;
    const double b =
        last == originPiece_ ? length_[last] - originLocal_ : (rel_[last] - originLocal_) + length_[last];
    return XSpan{a, b};
}

bool DistanceChain::indexedAt(Micros t) const noexcept {
    return !pieces_.empty() && lookups_[pieceAtTime(t)] != nullptr;
}

bool DistanceChain::coversTime(Micros t0, Micros t1) const noexcept {
    if (pieces_.empty() || t0 < pieces_.front().start || t1 > pieces_.back().end) {
        return false;
    }
    const std::size_t first = pieceAtTime(t0);
    const std::size_t last = t1 > t0 ? pieceAtTime(t1 - 1) : first;
    for (std::size_t p = first; p <= last; ++p) {
        if (!lookups_[p]) {
            return false;
        }
    }
    return true;
}

bool DistanceChain::coversDistance(double a, double b) const noexcept {
    if (pieces_.empty() || !std::isfinite(a) || !std::isfinite(b)) {
        return false;
    }
    const XSpan e = extent();
    if (a < e.a || b > e.b) {
        return false;
    }
    const double u0 = a + originLocal_;
    const double u1 = b + originLocal_;
    for (std::size_t p = 0; p < pieces_.size(); ++p) {
        if (lookups_[p]) {
            continue;
        }
        // A collapsed lap sits at R_p; it breaks the span when it lies inside it (or the span is that point).
        const double at = rel_[p];
        if ((at > u0 && at < u1) || (u0 == u1 && at == u0)) {
            return false;
        }
    }
    return true;
}

std::vector<std::pair<Micros, Micros>> DistanceChain::unindexedSpans() const {
    std::vector<std::pair<Micros, Micros>> spans;
    for (std::size_t p = 0; p < pieces_.size(); ++p) {
        if (!lookups_[p]) {
            if (!spans.empty() && spans.back().second == pieces_[p].start) {
                spans.back().second = pieces_[p].end;
            } else {
                spans.emplace_back(pieces_[p].start, pieces_[p].end);
            }
        }
    }
    return spans;
}

// ---- DomainMapper
// --------------------------------------------------------------------------------------------

DomainMapper::DomainMapper(Domain domain, RangeGeom range, OutingGeom outing,
                           std::shared_ptr<const DistanceChain> chain, Rate rate, Micros seriesT0)
    : domain_(domain), range_(range), outing_(outing), chain_(std::move(chain)), rate_(rate),
      seriesT0_(seriesT0) {}

bool DomainMapper::hasDistance() const noexcept {
    return chain_ != nullptr && chain_->anyIndexed();
}

double DomainMapper::xOf(Micros tAbs) const noexcept {
    if (domain_ == Domain::Time) {
        return static_cast<double>(tAbs - range_.start + range_.offsetUs);
    }
    return chain_ ? chain_->distAt(tAbs) + range_.offsetM : 0.0;
}

Micros DomainMapper::timeAt(double x) const noexcept {
    if (domain_ == Domain::Time) {
        if (!std::isfinite(x)) {
            return x > 0.0 ? kMicrosMax : kMicrosMin;
        }
        const double clamped = std::clamp(x, -9.0e18, 9.0e18);
        return saturatingSub(saturatingAdd(range_.start, static_cast<Micros>(std::llround(clamped))),
                             range_.offsetUs);
    }
    return chain_ ? chain_->timeAt(x - range_.offsetM) : range_.start;
}

double DomainMapper::sampleAt(double x) const noexcept {
    if (!rate_.valid()) {
        return 0.0;
    }
    const auto dt = static_cast<double>(timeAt(x) - seriesT0_);
    return dt * static_cast<double>(rate_.num) / (1e6 * static_cast<double>(rate_.den));
}

double DomainMapper::xOfSample(double i) const noexcept {
    if (!rate_.valid()) {
        return 0.0;
    }
    if (std::floor(i) == i && std::abs(i) < 9.0e15) {
        return xOf(rate_.timeAt(static_cast<SampleIdx>(i), seriesT0_)); // D-129's exact sample time
    }
    const double t = static_cast<double>(seriesT0_) +
                     i * 1e6 * static_cast<double>(rate_.den) / static_cast<double>(rate_.num);
    return xOf(static_cast<Micros>(std::llround(t)));
}

double DomainMapper::extent() const noexcept {
    if (domain_ == Domain::Time) {
        return static_cast<double>(range_.end - range_.start);
    }
    return chain_ ? chain_->distAt(range_.end) : 0.0;
}

XSpan DomainMapper::outingExtent() const noexcept {
    if (domain_ == Domain::Time) {
        return XSpan{static_cast<double>(outing_.start - range_.start),
                     static_cast<double>(outing_.end - range_.start)};
    }
    return chain_ ? chain_->extent() : XSpan{};
}

double DomainMapper::pxOf(double x, XSpan visible, double x0Px, double widthPx) noexcept {
    const double len = visible.length();
    if (!(len > 0.0)) {
        return x0Px;
    }
    return x0Px + (x - visible.a) / len * widthPx;
}

double DomainMapper::xOfPx(double px, XSpan visible, double x0Px, double widthPx) noexcept {
    if (!(widthPx > 0.0)) {
        return visible.a;
    }
    return visible.a + (px - x0Px) / widthPx * visible.length();
}

// ---- F9
// ------------------------------------------------------------------------------------------------------

double mainExtentIn(Domain domain, const RangeGeom& main, const DistanceChain* chain,
                    double noDistanceLength) noexcept {
    if (domain == Domain::Time) {
        return static_cast<double>(main.end - main.start);
    }
    if (chain != nullptr && chain->coversTime(main.start, main.end)) {
        return chain->distAt(main.end);
    }
    return noDistanceLength;
}

XSpan outingExtentIn(Domain domain, const RangeGeom& main, const OutingGeom& outing,
                     const DistanceChain* chain, double noDistanceLength) noexcept {
    const XSpan timeExtent{static_cast<double>(outing.start - main.start),
                           static_cast<double>(outing.end - main.start)};
    if (domain == Domain::Time) {
        return timeExtent;
    }
    if (chain != nullptr && chain->anyIndexed() && chain->coversTime(outing.start, outing.end)) {
        return chain->extent();
    }
    if (chain != nullptr && chain->anyIndexed() && chain->coversTime(main.start, main.end)) {
        // A partial chain still spans the outing in distance: collapsed laps have length 0.
        return chain->extent();
    }
    // The Main lap has no index (none at all, or still pending while other laps of the outing have one):
    // mainExtentIn falls back to noDistanceLength, so the outing must be in the same normalised units (a
    // partial chain's metres would clamp a fraction-rule span to a garbage range).
    const auto mainTime = static_cast<double>(main.end - main.start);
    if (!(mainTime > 0.0)) {
        return XSpan{0.0, noDistanceLength};
    }
    const double scale = noDistanceLength / mainTime;
    return XSpan{timeExtent.a * scale, timeExtent.b * scale};
}

ZoomConversion convertZoom(XSpan zoom, Domain from, Domain to, double fa, double fb, const RangeGeom& main,
                           const OutingGeom& outing, const DistanceChain* chain,
                           double noDistanceLength) noexcept {
    if (from == to) {
        return ZoomConversion{zoom, false};
    }
    ZoomConversion out;
    bool converted = false;
    if (chain != nullptr) {
        if (from == Domain::Time) {
            const Micros t0 = main.start + static_cast<Micros>(std::llround(zoom.a));
            const Micros t1 = main.start + static_cast<Micros>(std::llround(zoom.b));
            if (chain->coversTime(t0, t1)) {
                out.zoom = XSpan{chain->distAt(t0), chain->distAt(t1)};
                converted = true;
            }
        } else if (chain->coversDistance(zoom.a, zoom.b)) {
            out.zoom = XSpan{static_cast<double>(chain->timeAt(zoom.a) - main.start),
                             static_cast<double>(chain->timeAt(zoom.b) - main.start)};
            converted = true;
        }
    }
    if (!converted) {
        const double ext = mainExtentIn(to, main, chain, noDistanceLength);
        out.zoom = XSpan{fa * ext, fb * ext};
    }
    out.throughChain = converted;
    const XSpan limit = outingExtentIn(to, main, outing, chain, noDistanceLength);
    XSpan clamped{std::max(out.zoom.a, limit.a), std::min(out.zoom.b, limit.b)};
    if (!(clamped.b > clamped.a)) {
        clamped = limit;
    }
    out.zoom = clamped;
    return out;
}

} // namespace esses::plot
