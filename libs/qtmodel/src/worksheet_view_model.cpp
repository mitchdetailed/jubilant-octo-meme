// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetViewModel (06 section 4.5, 04 sections 2-3 and 8.5-8.6; D-016, D-018, D-133; AC-M1-09).

#include "range_lookup.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>

#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace esses::qt {

namespace {

constexpr double kNoDistanceLength = 1.0; // extent'(R) without an index and without a track map (04 3.4)
constexpr int kAnimationIntervalMs = 33;  // 30 Hz (04 section 8.5 item 7)
constexpr int kOffsetCommitMs = 250;      // 04 section 3.3 item 4
constexpr Micros kFallbackPeriodUs = 1000;

Micros roundMicros(double v) {
    if (!std::isfinite(v)) {
        return 0;
    }
    constexpr auto lo = static_cast<double>(std::numeric_limits<Micros>::min() / 2);
    constexpr auto hi = static_cast<double>(std::numeric_limits<Micros>::max() / 2);
    return static_cast<Micros>(std::llround(std::clamp(v, lo, hi)));
}

QString formatSeconds(double us) {
    return QString::number(us / 1e6, 'f', 3) + QStringLiteral(" s");
}

QString formatMetres(double m) {
    return QString::number(m, 'f', 1) + QStringLiteral(" m");
}

} // namespace

struct WorksheetViewModel::MainGeom {
    bool valid = false;
    std::shared_ptr<const model::DataSource> source;
    const model::DataLayer* layer = nullptr;
    model::Range range;
    plot::RangeGeom geom; // offsets excluded (04 section 3.4)
    plot::OutingGeom outing;
    std::shared_ptr<const plot::DistanceChain> chain;

    const plot::DistanceChain* chainPtr() const noexcept { return chain.get(); }
    double timeLength() const noexcept { return static_cast<double>(geom.end - geom.start); }
};

WorksheetViewModel::WorksheetViewModel(const project::WorksheetDef& def, WorkspaceViewModel& workspace,
                                       DataSelectionModel& selection, DistanceService& distance,
                                       QObject* parent)
    : QObject(parent), def_(def), workspace_(workspace), selection_(selection), distance_(distance) {
    state_.domain = def_.domain == "distance" ? plot::Domain::Distance : plot::Domain::Time;
    if (workspace_.activeWorksheet() == nullptr) {
        workspace_.setActiveWorksheet(this);
    }
    lastMain_ = selection_.snapshot().main;
    connect(&workspace_, &WorkspaceViewModel::changed, this, &WorksheetViewModel::onWorkspaceChanged);
    connect(&selection_, &DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot& snapshot, DataSelectionModel::Changes) {
                onSelectionChanged(snapshot);
            });
    const auto onDistance = [this](const model::SourceId& src) {
        if (selection_.hasMain() && selection_.snapshot().main.src == src) {
            ++updating_;
            onMainIndexArrived();
            --updating_;
            const plot::ChangeMask what = deferred_ | plot::Change::Data;
            deferred_ = 0;
            updateBanner();
            compose(what);
        }
    };
    connect(&distance_, &DistanceService::distanceReady, this, onDistance);
    connect(&distance_, &DistanceService::noDistance, this, onDistance);
    animationTimer_ = new QTimer(this);
    animationTimer_->setInterval(kAnimationIntervalMs);
    connect(animationTimer_, &QTimer::timeout, this, &WorksheetViewModel::animationTick);
    offsetTimer_ = new QTimer(this);
    offsetTimer_->setSingleShot(true);
    offsetTimer_->setInterval(kOffsetCommitMs);
    connect(offsetTimer_, &QTimer::timeout, this, &WorksheetViewModel::commitOffsets);
    // Session start: zoom = the Main range (W), cursor = its start (06 section 4.1 item 4).
    if (isActive() && selection_.hasMain() && !(workspace_.zoom().length() > 0.0)) {
        ++updating_;
        zoomToMainRange();
        workspace_.setCursor(0);
        --updating_;
        deferred_ = 0;
    }
    compose(0);
    updateBanner();
}

WorksheetViewModel::~WorksheetViewModel() {
    if (workspace_.activeWorksheet() == this) {
        workspace_.setActiveWorksheet(nullptr);
    }
}

bool WorksheetViewModel::isActive() const noexcept {
    return workspace_.activeWorksheet() == this;
}

bool WorksheetViewModel::animating() const noexcept {
    return animationTimer_ != nullptr && animationTimer_->isActive();
}

// ---- geometry -------------------------------------------------------------------------------------------

WorksheetViewModel::MainGeom WorksheetViewModel::mainGeom(const model::RangeRef& main) const {
    MainGeom m;
    if (main.range.v.isNil()) {
        return m;
    }
    m.source = selection_.source(main.src);
    if (m.source == nullptr) {
        return m;
    }
    const model::Range* r = detail::findRange(*m.source, main);
    m.layer = detail::layerOf(*m.source, main.layer);
    if (r == nullptr || m.layer == nullptr || r->end <= r->start) {
        return m;
    }
    m.range = *r;
    m.geom = plot::RangeGeom{r->start, r->end, 0, 0.0, r->stretch.value_or(1.0)};
    m.outing = r->kind == model::RangeKind::Outing ? plot::OutingGeom{r->start, r->end}
                                                   : detail::outingAt(*m.layer, r->start);
    m.chain = distance_.chain(main, Priority::Visible);
    m.valid = true;
    return m;
}

WorksheetViewModel::MainGeom WorksheetViewModel::mainGeom() const {
    return mainGeom(selection_.snapshot().main);
}

Micros WorksheetViewModel::activeSamplePeriod() const {
    if (samplePeriodUs_ > 0) {
        return samplePeriodUs_;
    }
    const plot::SelectionSnapshot& s = selection_.snapshot();
    const auto src = selection_.source(s.main.src);
    const model::DataLayer* layer = src == nullptr ? nullptr : detail::layerOf(*src, s.main.layer);
    double fastest = 0.0;
    if (layer != nullptr) {
        for (const model::Channel& c : layer->channels) {
            if (c.desc.rate && c.desc.rate->valid()) {
                fastest = std::max(fastest, c.desc.rate->hz());
            }
        }
    }
    return fastest > 0.0 ? std::max<Micros>(1, roundMicros(1e6 / fastest)) : kFallbackPeriodUs;
}

double WorksheetViewModel::xOfT(const MainGeom& m, plot::Domain domain, Micros t) const {
    if (domain == plot::Domain::Time || !m.valid) {
        return static_cast<double>(t);
    }
    const Micros abs = m.geom.start + t;
    if (m.chain != nullptr && m.chain->indexedAt(abs)) {
        return m.chain->distAt(abs);
    }
    // The fraction rule (D-016) when the lap has no index.
    const double len = m.timeLength();
    return len > 0.0 ? static_cast<double>(t) / len *
                           plot::mainExtentIn(domain, m.geom, m.chainPtr(), kNoDistanceLength)
                     : 0.0;
}

Micros WorksheetViewModel::tOfX(const MainGeom& m, plot::Domain domain, double x) const {
    if (domain == plot::Domain::Time || !m.valid) {
        return roundMicros(x);
    }
    if (m.chain != nullptr && m.chain->anyIndexed()) {
        const plot::XSpan ext = m.chain->extent();
        if (x >= ext.a && x <= ext.b) {
            const Micros t = m.chain->timeAt(x);
            if (m.chain->indexedAt(t)) {
                return t - m.geom.start;
            }
        }
    }
    const double extent = plot::mainExtentIn(domain, m.geom, m.chainPtr(), kNoDistanceLength);
    return extent > 0.0 ? roundMicros(x / extent * m.timeLength()) : 0;
}

double WorksheetViewModel::minSpan(const MainGeom& m, plot::Domain domain) const {
    const auto minTime = static_cast<double>(4 * activeSamplePeriod());
    if (domain == plot::Domain::Time || !m.valid) {
        return minTime;
    }
    if (m.chain != nullptr && m.chain->anyIndexed()) {
        return 4.0 * analysis::DistanceIndex::kStepM;
    }
    const double len = m.timeLength();
    return len > 0.0 ? minTime / len * plot::mainExtentIn(domain, m.geom, m.chainPtr(), kNoDistanceLength)
                     : minTime;
}

Micros WorksheetViewModel::clampCursor(const MainGeom& m, Micros t) const {
    if (!m.valid) {
        return t;
    }
    const Micros lo = m.outing.start - m.geom.start;
    const Micros hi = std::max(lo, m.outing.end - m.geom.start - 1);
    return std::clamp(t, lo, hi);
}

plot::XSpan WorksheetViewModel::zoomIn(plot::Domain domain) const {
    if (workspace_.zoomDomain() == domain) {
        return workspace_.zoom();
    }
    const MainGeom m = mainGeom();
    if (!m.valid) {
        return workspace_.zoom();
    }
    const ZoomFraction f = workspace_.zoomFrac();
    return plot::convertZoom(workspace_.zoom(), workspace_.zoomDomain(), domain, f.fa, f.fb, m.geom, m.outing,
                             m.chainPtr(), kNoDistanceLength)
        .zoom;
}

// ---- composition ----------------------------------------------------------------------------------------

void WorksheetViewModel::compose(plot::ChangeMask what) {
    plot::ViewState next = state_;
    next.cursorT = workspace_.cursorT();
    next.datumT = workspace_.datumT();
    next.zoom = zoomIn(state_.domain);
    what = static_cast<plot::ChangeMask>(what | plot::viewChanges(state_, next));
    if (what == 0) {
        return;
    }
    next.version = state_.version + 1;
    state_ = next;
    if (plot::has(what, plot::Change::Cursor) || plot::has(what, plot::Change::Datum)) {
        emitCursorInfo();
    }
    Q_EMIT changed(what);
}

void WorksheetViewModel::onWorkspaceChanged(plot::ChangeMask what) {
    if (updating_ > 0) {
        deferred_ = static_cast<plot::ChangeMask>(deferred_ | what);
        return;
    }
    compose(what);
}

void WorksheetViewModel::updateBanner() {
    const bool missing = state_.domain == plot::Domain::Distance && selection_.hasMain() &&
                         distance_.state(selection_.snapshot().main.src) == DistanceState::NoDistance;
    if (missing != noDistance_) {
        noDistance_ = missing;
        Q_EMIT noDistanceData(missing);
    }
}

void WorksheetViewModel::emitCursorInfo() {
    const MainGeom m = mainGeom();
    if (!m.valid) {
        return;
    }
    const Micros t = workspace_.cursorT();
    const Micros abs = m.geom.start + t;
    const bool hasDistance = m.chain != nullptr && m.chain->indexedAt(abs);
    QString lap;
    if (const model::RangeGroup* g = m.layer->ranges.find(detail::kLapsGroup)) {
        for (const model::Range& r : g->ranges) {
            if (abs >= r.start && abs < r.end) {
                lap = QString::fromStdString(r.name);
                break;
            }
        }
    }
    QString dt;
    QString dd;
    if (const std::optional<Micros> d = workspace_.datumT()) {
        dt = formatSeconds(static_cast<double>(t - *d));
        if (hasDistance && m.chain->indexedAt(m.geom.start + *d)) {
            dd = formatMetres(m.chain->distAt(abs) - m.chain->distAt(m.geom.start + *d));
        }
    }
    Q_EMIT cursorInfo(formatSeconds(static_cast<double>(t)),
                      hasDistance ? formatMetres(m.chain->distAt(abs)) : QString(), lap, dt, dd);
}

// ---- domain ---------------------------------------------------------------------------------------------

void WorksheetViewModel::convertSharedZoom(plot::Domain to) {
    const plot::Domain from = workspace_.zoomDomain();
    if (from == to) {
        return;
    }
    const MainGeom m = mainGeom();
    if (!m.valid) {
        workspace_.convertZoom(to, 0.0, workspace_.zoom());
        noteDistanceUnits(m);
        return;
    }
    const ZoomFraction f = workspace_.zoomFrac();
    const plot::ZoomConversion conv = plot::convertZoom(workspace_.zoom(), from, to, f.fa, f.fb, m.geom,
                                                        m.outing, m.chainPtr(), kNoDistanceLength);
    workspace_.convertZoom(to, plot::mainExtentIn(to, m.geom, m.chainPtr(), kNoDistanceLength), conv.zoom);
    noteDistanceUnits(m);
}

bool WorksheetViewModel::coversMain(const MainGeom& m) const {
    return m.valid && m.chain != nullptr && m.chain->coversTime(m.geom.start, m.geom.end);
}

// After the shared zoom was set from Main extents (F9, W, F2, the first Main): metres when the Main lap has
// its index, else the fraction rule's normalised units (D-016).
void WorksheetViewModel::noteDistanceUnits(const MainGeom& m) {
    workspace_.setZoomNormalised(workspace_.zoomDomain() == plot::Domain::Distance && m.valid &&
                                 !coversMain(m));
}

// The Main source delivered a distance index. A distance zoom set while the Main lap's index was pending is
// in normalised units: re-express it in metres with the same zoomFrac (otherwise F9 or W pressed in the first
// moments after an open leaves a span of a fraction of a metre). A metre zoom kept across N/P only gets its
// zoomFrac recomputed against the new lap's length. Active worksheet only (it owns the shared zoom).
void WorksheetViewModel::onMainIndexArrived() {
    if (!isActive() || workspace_.zoomDomain() != plot::Domain::Distance) {
        return;
    }
    const MainGeom m = mainGeom();
    if (!coversMain(m)) {
        return;
    }
    const double ext = plot::mainExtentIn(plot::Domain::Distance, m.geom, m.chainPtr(), kNoDistanceLength);
    if (workspace_.zoomNormalised()) {
        const ZoomFraction f = workspace_.zoomFrac();
        workspace_.setZoomNormalised(false);
        const plot::Domain domain = state_.domain;
        state_.domain = plot::Domain::Distance; // applyZoom clamps in the shared zoom's domain
        applyZoom(plot::XSpan{f.fa * ext, f.fb * ext});
        state_.domain = domain;
    } else {
        workspace_.setZoom(workspace_.zoom(), plot::Domain::Distance, ext);
    }
}

void WorksheetViewModel::activate() {
    workspace_.setActiveWorksheet(this);
    ++updating_;
    convertSharedZoom(state_.domain);
    --updating_;
    const plot::ChangeMask what = deferred_;
    deferred_ = 0;
    compose(what);
}

void WorksheetViewModel::setDomain(plot::Domain domain) {
    if (state_.domain == domain) {
        return;
    }
    ++updating_;
    if (isActive()) {
        convertSharedZoom(domain); // cursor and datum are lap times: unchanged (04 section 3.4)
    }
    --updating_;
    state_.domain = domain;
    deferred_ = 0;
    compose(plot::mask(plot::Change::Domain)); // the domain is already switched: force its bit
    updateBanner();
}

void WorksheetViewModel::toggleDomain() {
    setDomain(state_.domain == plot::Domain::Time ? plot::Domain::Distance : plot::Domain::Time);
}

// ---- zoom and pan (04 section 8.6) ----------------------------------------------------------------------

void WorksheetViewModel::applyZoom(plot::XSpan span) {
    const plot::Domain d = state_.domain;
    const MainGeom m = mainGeom();
    if (!m.valid) {
        workspace_.setZoom(span, d, 0.0);
        return;
    }
    const plot::XSpan outing = plot::outingExtentIn(d, m.geom, m.outing, m.chainPtr(), kNoDistanceLength);
    // Minimum 4 samples of the active trace, maximum the Main outing (F2), clamped to the outing, never to 0.
    const double lo = minSpan(m, d);
    double len = span.length();
    double a = span.a;
    if (!(len >= lo)) {
        a = (span.a + span.b) / 2.0 - lo / 2.0; // grow about the centre
        len = lo;
    }
    if (outing.length() > 0.0) {
        len = std::min(len, std::max(outing.length(), lo));
        a = std::clamp(a, outing.a, std::max(outing.a, outing.b - len));
    }
    const double extent = plot::mainExtentIn(d, m.geom, m.chainPtr(), kNoDistanceLength);
    workspace_.setZoom(plot::XSpan{a, a + len}, d, extent);
}

void WorksheetViewModel::zoomIn() {
    const MainGeom m = mainGeom();
    const plot::XSpan z = state_.zoom;
    const double c = std::clamp(xOfT(m, state_.domain, state_.cursorT), z.a, z.b);
    applyZoom(plot::XSpan{c - (c - z.a) * 0.5, c + (z.b - c) * 0.5});
}

void WorksheetViewModel::zoomOut() {
    const MainGeom m = mainGeom();
    const plot::XSpan z = state_.zoom;
    const double c = std::clamp(xOfT(m, state_.domain, state_.cursorT), z.a, z.b);
    applyZoom(plot::XSpan{c - (c - z.a) * 2.0, c + (z.b - c) * 2.0});
}

void WorksheetViewModel::zoomFull() {
    const MainGeom m = mainGeom();
    if (!m.valid) {
        return;
    }
    applyZoom(plot::outingExtentIn(state_.domain, m.geom, m.outing, m.chainPtr(), kNoDistanceLength));
    noteDistanceUnits(m);
}

void WorksheetViewModel::zoomToMainRange() {
    const MainGeom m = mainGeom();
    if (!m.valid) {
        return;
    }
    ++updating_;
    applyZoom(plot::XSpan{0.0, plot::mainExtentIn(state_.domain, m.geom, m.chainPtr(), kNoDistanceLength)});
    noteDistanceUnits(m);
    const Micros t = workspace_.cursorT();
    if (t < 0 || t >= m.geom.end - m.geom.start) {
        workspace_.setCursor(0); // W pulls the cursor inside (04 section 8.6)
    }
    --updating_;
    const plot::ChangeMask what = deferred_;
    deferred_ = 0;
    compose(what);
}

void WorksheetViewModel::zoomToDatumMain() {
    const std::optional<Micros> datum = workspace_.datumT();
    if (!datum) {
        return; // Z requires a datum
    }
    const MainGeom m = mainGeom();
    const double xc = xOfT(m, state_.domain, workspace_.cursorT());
    const double xd = xOfT(m, state_.domain, *datum);
    if (xc == xd) {
        return;
    }
    applyZoom(plot::XSpan{std::min(xc, xd), std::max(xc, xd)});
}

void WorksheetViewModel::zoomTo(plot::XSpan span) {
    if (span.b < span.a) {
        std::swap(span.a, span.b);
    }
    applyZoom(span);
}

void WorksheetViewModel::extendZoom(double x) {
    const plot::XSpan z = state_.zoom;
    applyZoom(plot::XSpan{std::min(z.a, x), std::max(z.b, x)});
}

void WorksheetViewModel::panBy(double fractionOfSpan) {
    const plot::XSpan z = state_.zoom;
    const double d = z.length() * fractionOfSpan;
    applyZoom(plot::XSpan{z.a + d, z.b + d});
}

void WorksheetViewModel::centreOnCursor() {
    const MainGeom m = mainGeom();
    const plot::XSpan z = state_.zoom;
    const double c = xOfT(m, state_.domain, workspace_.cursorT());
    const double half = z.length() / 2.0;
    applyZoom(plot::XSpan{c - half, c + half});
}

// ---- cursor and datum (04 section 8.5) ------------------------------------------------------------------

void WorksheetViewModel::setCursor(Micros tMainLap, CursorSource source) {
    const MainGeom m = mainGeom();
    ++updating_;
    workspace_.setCursor(clampCursor(m, tMainLap));
    if (autoCentre_ && source != CursorSource::Programmatic) {
        centreOnCursor();
    }
    --updating_;
    const plot::ChangeMask what = deferred_;
    deferred_ = 0;
    compose(what);
}

void WorksheetViewModel::stepCursor(int screenPixels) {
    if (screenPixels == 0) {
        return;
    }
    const MainGeom m = mainGeom();
    const Micros period = activeSamplePeriod();
    const double width = std::max(1, widthPx_);
    const Micros t = workspace_.cursorT();
    Micros next = t;
    if (state_.domain == plot::Domain::Time) {
        const double spp = state_.zoom.length() / width / static_cast<double>(period);
        const auto perPixel = std::max<Micros>(1, roundMicros(spp));
        next = t + perPixel * screenPixels * period;
    } else {
        const double dx = state_.zoom.length() / width * screenPixels;
        next = tOfX(m, state_.domain, xOfT(m, state_.domain, t) + dx);
        if (screenPixels > 0 ? next <= t : next >= t) {
            next = t + (screenPixels > 0 ? period : -period);
        }
    }
    setCursor(next, CursorSource::Keyboard);
}

void WorksheetViewModel::cursorHome() {
    const MainGeom m = mainGeom();
    setCursor(tOfX(m, state_.domain, state_.zoom.a), CursorSource::Keyboard);
}

void WorksheetViewModel::cursorEnd() {
    const MainGeom m = mainGeom();
    const Micros a = tOfX(m, state_.domain, state_.zoom.a);
    const Micros b = tOfX(m, state_.domain, state_.zoom.b) - activeSamplePeriod();
    setCursor(std::max(a, b), CursorSource::Keyboard);
}

void WorksheetViewModel::placeDatum() {
    workspace_.setDatum(workspace_.cursorT());
}

void WorksheetViewModel::toggleDatum() {
    if (!workspace_.storedDatumT()) {
        workspace_.setDatum(workspace_.cursorT());
        return;
    }
    workspace_.setDatumVisible(!workspace_.datumVisible());
}

void WorksheetViewModel::swapDatum() {
    if (workspace_.datumT()) {
        workspace_.swapCursorDatum();
    }
}

// ---- offsets (04 section 3.3) ---------------------------------------------------------------------------

void WorksheetViewModel::setOffsetAxis(bool on) {
    if (state_.offsetAxis == on) {
        return;
    }
    state_.offsetAxis = on;
    if (!on) {
        // Turning the axis off clears the offsets of every selected range ("Clear alignment offsets").
        offsetTimer_->stop();
        pendingOffsets_.clear();
        if (offsetCommit_) {
            // Every selected range, the overlays included while F4 hides them (06 section 14.4).
            const plot::SelectionSnapshot& s = selection_.snapshot();
            std::vector<model::RangeRef> ranges{s.main};
            if (const std::optional<model::RangeRef>& o0 = s.overlay0) {
                ranges.push_back(*o0);
            }
            ranges.insert(ranges.end(), s.overlays.begin(), s.overlays.end());
            for (const model::RangeRef& range : ranges) {
                if (const model::Range* r = selection_.resolve(range)) {
                    if (r->offsetUs != 0) {
                        offsetCommit_(range, plot::Domain::Time, 0.0);
                    }
                    if (r->offsetM != 0.0) {
                        offsetCommit_(range, plot::Domain::Distance, 0.0);
                    }
                }
            }
        }
    }
    ++state_.version;
    Q_EMIT changed(plot::mask(plot::Change::Offsets));
}

void WorksheetViewModel::offsetRange(const model::RangeRef& r, int steps) {
    const double step = state_.domain == plot::Domain::Time ? static_cast<double>(samplePeriodOf(r.src))
                                                            : analysis::DistanceIndex::kStepM;
    dragOffset(r, step * steps);
}

Micros WorksheetViewModel::samplePeriodOf(const model::SourceId& source) const {
    const auto it = sourcePeriods_.find(source);
    return it != sourcePeriods_.end() && it->second > 0 ? it->second : activeSamplePeriod();
}

void WorksheetViewModel::setSourceSamplePeriod(const model::SourceId& source, Micros periodUs) {
    if (periodUs > 0) {
        sourcePeriods_[source] = periodUs;
    } else {
        sourcePeriods_.erase(source);
    }
}

void WorksheetViewModel::offsetRangeBy(const model::RangeRef& r, double amount) {
    dragOffset(r, amount);
}

void WorksheetViewModel::beginOffsetDrag() {
    ++offsetDragDepth_;
    offsetTimer_->stop();
}

void WorksheetViewModel::endOffsetDrag() {
    if (offsetDragDepth_ > 0) {
        --offsetDragDepth_;
    }
    if (offsetDragDepth_ == 0) {
        offsetTimer_->stop();
        commitOffsets();
    }
}

void WorksheetViewModel::dragOffset(const model::RangeRef& r, double delta) {
    const model::Range* range = selection_.resolve(r);
    if (range == nullptr || delta == 0.0) {
        return;
    }
    auto it = pendingOffsets_.find(r);
    if (it == pendingOffsets_.end()) {
        const double stored =
            state_.domain == plot::Domain::Time ? static_cast<double>(range->offsetUs) : range->offsetM;
        it = pendingOffsets_.emplace(r, stored).first;
    }
    it->second += delta;
    state_.offsetAxis = true;
    ++state_.version;
    if (offsetDragDepth_ == 0) {
        offsetTimer_->start();
    }
    Q_EMIT changed(plot::mask(plot::Change::Offsets));
}

void WorksheetViewModel::commitOffsets() {
    if (pendingOffsets_.empty()) {
        return;
    }
    const auto pending = std::move(pendingOffsets_);
    pendingOffsets_.clear();
    if (offsetCommit_) {
        for (const auto& [range, value] : pending) {
            offsetCommit_(range, state_.domain, value);
        }
    }
    ++state_.version;
    Q_EMIT changed(plot::mask(plot::Change::Offsets));
}

// ---- selection ------------------------------------------------------------------------------------------

void WorksheetViewModel::onSelectionChanged(const plot::SelectionSnapshot& snapshot) {
    plot::ChangeMask what = plot::mask(plot::Change::Selection);
    const model::RangeRef& main = snapshot.main;
    const bool mainChanged = !(main == lastMain_) || main.range.v.isNil() != lastMain_.range.v.isNil();
    if (mainChanged) {
        const model::RangeRef oldMain = lastMain_;
        lastMain_ = main;
        ++updating_;
        if (isActive()) {
            const MainGeom before = mainGeom(oldMain);
            const MainGeom after = mainGeom(main);
            if (!before.valid && after.valid) {
                // First Main of the session: W and the cursor at the range start (06 section 4.1 item 4).
                workspace_.setCursorDatum(0, workspace_.storedDatumT());
                applyZoom(plot::XSpan{
                    0.0, plot::mainExtentIn(state_.domain, after.geom, after.chainPtr(), kNoDistanceLength)});
                noteDistanceUnits(after);
            } else if (after.valid) {
                // N/P/ENTER/Q: zoom kept relative to the range start; cursor and datum keep their place in
                // this worksheet's domain (04 section 2.1 item 1), then clamp to the new Main outing.
                const auto convert = [&](Micros t) {
                    Micros out = t;
                    if (state_.domain == plot::Domain::Distance && before.valid) {
                        out = tOfX(after, state_.domain, xOfT(before, state_.domain, t));
                    }
                    return clampCursor(after, out);
                };
                const std::optional<Micros> datum = workspace_.storedDatumT();
                workspace_.setCursorDatum(convert(workspace_.cursorT()),
                                          datum ? std::optional<Micros>(convert(*datum)) : std::nullopt);
                // zoomFrac follows the new Main extent so the F9 fallback uses |R'| (D-016). In distance the
                // units must match the new Main lap: normalised -> metres once it has an index; metres kept
                // while its index is pending (zoomFrac refreshed when it arrives); metres -> the fraction
                // rule for a source without distance data.
                const plot::Domain zd = workspace_.zoomDomain();
                const ZoomFraction f = workspace_.zoomFrac();
                const double ext = plot::mainExtentIn(zd, after.geom, after.chainPtr(), kNoDistanceLength);
                if (zd == plot::Domain::Distance && workspace_.zoomNormalised() && coversMain(after)) {
                    workspace_.setZoom(plot::XSpan{f.fa * ext, f.fb * ext}, zd, ext);
                    workspace_.setZoomNormalised(false);
                } else if (zd == plot::Domain::Distance && !workspace_.zoomNormalised() &&
                           !coversMain(after)) {
                    if (distance_.state(main.src) == DistanceState::NoDistance) {
                        workspace_.setZoom(plot::XSpan{f.fa * ext, f.fb * ext}, zd, ext);
                        workspace_.setZoomNormalised(true);
                    }
                } else {
                    workspace_.setZoom(workspace_.zoom(), zd, ext);
                }
            }
        }
        --updating_;
        what = static_cast<plot::ChangeMask>(what | deferred_);
        deferred_ = 0;
        // Selecting a range with a stored offset shows the offset axis (D-016, 04 section 3.3 item 5).
        if (const model::Range* r = selection_.resolve(main);
            r != nullptr && !state_.offsetAxis && (r->offsetUs != 0 || r->offsetM != 0.0)) {
            state_.offsetAxis = true;
            what = what | plot::Change::Offsets;
        }
        what = what | plot::Change::Data;
    }
    compose(what);
    updateBanner();
}

// ---- animation (04 section 8.5 item 7) ------------------------------------------------------------------

void WorksheetViewModel::setAnimating(bool on) {
    if (on == animating()) {
        return;
    }
    if (on) {
        animationCarryUs_ = 0.0;
        animationClock_.start();
        animationTimer_->start();
    } else {
        animationTimer_->stop();
    }
}

void WorksheetViewModel::setAnimationRate(double x) {
    animationRate_ = std::clamp(x, 0.25, 10.0); // Data -> Animation Rate, x0.25 to x10 (D-203 (g))
}

void WorksheetViewModel::animationTick() {
    const qint64 elapsedNs = animationClock_.nsecsElapsed();
    animationClock_.restart();
    animationCarryUs_ += static_cast<double>(elapsedNs) / 1000.0 * animationRate_;
    const auto step = static_cast<Micros>(animationCarryUs_);
    if (step <= 0) {
        return;
    }
    animationCarryUs_ -= static_cast<double>(step);
    const MainGeom m = mainGeom();
    const Micros t = workspace_.cursorT() + step;
    const Micros clamped = clampCursor(m, t);
    setCursor(clamped, CursorSource::Animation);
    if (clamped != t) {
        setAnimating(false); // the end of the outing
    }
}

void WorksheetViewModel::setSize(int widthPx) {
    if (widthPx_ == widthPx) {
        return;
    }
    widthPx_ = widthPx;
    ++state_.version;
    Q_EMIT changed(plot::mask(plot::Change::Size));
}

} // namespace esses::qt
