// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DistanceService (06 section 4.5 constructor argument of WorksheetViewModel; 04 section 3.1; D-016, D-134,
// D-140; 01 section 5.2 "DistanceIndex build per (source, lap)"). Added to esses_qtmodel in M1: 06 names it
// without declaring it. It hands out the per-lap analysis::DistanceIndex and the outing-level
// plot::DistanceChain of a range without blocking: an absent index is requested on a worker (share key
// distidx/<sourceId>/<layer>/<rangeId>, at Visible for a trace build, Background for prefetch) from the
// source's Corr Dist (through MathsService) and published as std::shared_ptr<const ...>; distanceReady()
// fires through the Dispatcher. Indexes are cached by the D-140 key (Corr Dist column version, lap marker ids
// and times, stretch), never by DataSource::version. GUI thread only.
// M1 notes: the task key is distidx/<sourceId>/<layer>/<rangeId>/<Corr Dist version> (the version suffix
// keeps a stale build from being joined after Corr Dist was re-evaluated); a lap whose Corr Dist has no valid
// sample stays without an index (the chain draws only the indexed laps); state() is NoDistance when the
// source has no usable Corr Dist (no speed channel, E-CSD-NO-SPEED) or no lap could be indexed, Ready once
// one index exists. Laps requested before Corr Dist is evaluated are built when MathsService::channelReady
// arrives.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/base/priority.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/range.hpp>
#include <esses/plotcore/domain_mapper.hpp>

#include <QObject>
#include <memory>

namespace esses::qt {

class Dispatcher;
class MathsService;

enum class DistanceState : std::uint8_t { Unknown, Pending, Ready, NoDistance };

class DistanceService final : public QObject {
    Q_OBJECT
public:
    DistanceService(MathsService& maths, Dispatcher& dispatcher, QObject* parent = nullptr);
    ~DistanceService() override;

    // Non-blocking; nullptr while pending (a build is requested at `priority`) or when the source has no
    // Corr Dist.
    std::shared_ptr<const analysis::DistanceIndex> index(const model::RangeRef& lap, Priority priority);
    // The outing chain around `range` (origin at range.start); nullptr when no lap of the outing has an
    // index.
    std::shared_ptr<const plot::DistanceChain> chain(const model::RangeRef& range, Priority priority);
    DistanceState state(const model::SourceId& source) const;
    // Background prefetch of every lap index of a source after open (AC-M1-10).
    void prefetch(const model::SourceId& source);
    void forgetSource(const model::SourceId& source);

Q_SIGNALS:
    void distanceReady(const esses::model::SourceId& source);
    void noDistance(const esses::model::SourceId& source);

private:
    struct Impl;
    void onCorrDistReady(const model::SourceId& source);
    void onStatusChanged(const model::SourceId& source);
    void prefetchLaps(const model::SourceId& source);
    void setState(const model::SourceId& source, DistanceState state);

    MathsService& maths_;
    Dispatcher& dispatcher_;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::qt
