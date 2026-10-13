// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DistanceService (06 section 4.5, 04 section 3.1, 01 section 5.2; D-016, D-134, D-140; AC-M1-08/09/10): the
// per-lap DistanceIndex built on a worker from the source's Corr Dist and the outing-level DistanceChain.

#include "range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QPointer>
#include <algorithm>
#include <bit>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

using IndexPtr = std::shared_ptr<const analysis::DistanceIndex>;

std::uint64_t foldVersion(const maths::Version& v) noexcept {
    return v.lo ^ (v.hi * 0x9E3779B97F4A7C15ull);
}

// The parts of a DistanceIndexKey beyond the Corr Dist version (lap bounds, anchor markers, stretch), so two
// requests for one range id with different keys never share a task.
std::string keyHash(const analysis::DistanceIndexKey& key) {
    Hash128Stream h;
    h.updatePart("esses.distidx-1");
    h.updateU64(key.corrDistVersion);
    h.updatePart(key.startMarker.str());
    h.updatePart(key.endMarker.str());
    h.updateU64(static_cast<std::uint64_t>(key.start));
    h.updateU64(static_cast<std::uint64_t>(key.end));
    h.updateU64(std::bit_cast<std::uint64_t>(key.stretch));
    return h.digest().hex();
}

bool sameButStretch(const analysis::DistanceIndexKey& a, const analysis::DistanceIndexKey& b) {
    analysis::DistanceIndexKey c = a;
    c.stretch = b.stretch;
    return c == b;
}

bool isCorrDist(const maths::ChannelKey& key) {
    return key.nameCf == caseFold(detail::kCorrDist);
}

model::RangeRef normalised(model::RangeRef r) {
    r.layer = std::string(model::canonicalLayerName(r.layer));
    r.name.clear();
    return r;
}

} // namespace

struct DistanceService::Impl {
    struct Lap {
        analysis::DistanceIndexKey key;
        IndexPtr index;
        bool pending = false;
        bool failed = false;
        TaskKey taskKey;                          // of the pending build
        Priority priority = Priority::Background; // of the pending build (raised by a more urgent request)
    };
    struct Chain {
        std::uint64_t generation = 0;
        std::uint64_t sourceVersion = 0;
        std::shared_ptr<const plot::DistanceChain> chain;
    };
    struct Source {
        DistanceState state = DistanceState::Unknown;
        bool prefetch = false;
        std::uint64_t generation = 0; // bumps whenever an index of the source is stored or fails
        std::unordered_map<model::RangeRef, Lap> laps;
        std::unordered_map<model::RangeRef, Chain> chains;
        std::vector<std::pair<model::RangeRef, Priority>> waiting; // requested before Corr Dist was ready
    };
    std::map<model::SourceId, Source> sources;
};

DistanceService::DistanceService(MathsService& maths, Dispatcher& dispatcher, QObject* parent)
    : QObject(parent), maths_(maths), dispatcher_(dispatcher), impl_(std::make_unique<Impl>()) {
    connect(&maths_, &MathsService::channelReady, this, [this](const maths::ChannelKey& key) {
        if (isCorrDist(key)) {
            onCorrDistReady(key.src);
        }
    });
    connect(&maths_, &MathsService::statusChanged, this, [this](const maths::ChannelKey& key) {
        if (isCorrDist(key)) {
            onStatusChanged(key.src);
        }
    });
}

DistanceService::~DistanceService() {
    for (const auto& entry : impl_->sources) {
        maths_.pool().cancelAll("distidx/" + detail::sourceHex(entry.first) + "/");
    }
}

void DistanceService::setState(const model::SourceId& source, DistanceState state) {
    Impl::Source& s = impl_->sources[source];
    if (s.state == state) {
        return;
    }
    s.state = state;
    if (state == DistanceState::NoDistance) {
        s.waiting.clear();
        Q_EMIT noDistance(source);
    }
}

std::shared_ptr<const analysis::DistanceIndex> DistanceService::index(const model::RangeRef& lapRef,
                                                                      Priority priority) {
    const auto src = maths_.source(lapRef.src);
    const model::Range* range = src == nullptr ? nullptr : detail::findRange(*src, lapRef);
    if (range == nullptr || range->end <= range->start) {
        return nullptr;
    }
    const model::RangeRef lap = normalised(lapRef);
    Impl::Source& s = impl_->sources[lap.src];
    if (!maths_.isMathsChannel(lap.src, lap.layer, detail::kCorrDist)) {
        setState(lap.src, DistanceState::NoDistance);
        return nullptr;
    }
    const maths::ChannelKey corrKey = maths::ChannelKey::make(lap.src, lap.layer, detail::kCorrDist);
    const maths::MathState st = maths_.status(corrKey).state;
    if (st == maths::MathState::Error || st == maths::MathState::Disabled) {
        setState(lap.src, DistanceState::NoDistance);
        return nullptr;
    }
    const std::optional<maths::ColumnHandle> corrDist = maths_.require(corrKey, priority);
    if (!corrDist) {
        const auto known =
            std::find_if(s.waiting.begin(), s.waiting.end(), [&](const auto& w) { return w.first == lap; });
        if (known == s.waiting.end()) {
            s.waiting.emplace_back(lap, priority);
        } else if (static_cast<int>(priority) < static_cast<int>(known->second)) {
            known->second = priority;
        }
        if (s.state == DistanceState::Unknown) {
            s.state = DistanceState::Pending;
        }
        return nullptr;
    }
    analysis::DistanceIndexKey key;
    key.corrDistVersion = foldVersion(corrDist->version);
    key.startMarker = range->startMarker;
    key.endMarker = range->endMarker;
    key.start = range->start;
    key.end = range->end;
    key.stretch = range->stretch.value_or(1.0);
    Impl::Lap& entry = s.laps[lap];
    if (entry.key == key) {
        if (entry.pending && static_cast<int>(priority) < static_cast<int>(entry.priority)) {
            // A queued build (e.g. the Background prefetch at publication) asked for again more urgently: it
            // moves to the higher band instead of waiting behind every Background task.
            maths_.pool().raise(entry.taskKey, priority);
            entry.priority = priority;
        }
        if (entry.index != nullptr || entry.failed || entry.pending) {
            return entry.index;
        }
    }
    if (entry.index != nullptr && !entry.pending && sameButStretch(entry.key, key)) {
        // Only the lap-stretch factor changed (LapStretchService): the published index is copied with the new
        // scalar (D-140: a shared index never changes), without a rebuild from Corr Dist.
        auto copy = std::make_shared<analysis::DistanceIndex>(*entry.index);
        copy->setStretch(key.stretch);
        entry = Impl::Lap{key, std::move(copy), false, false, {}, priority};
        ++s.generation;
        return entry.index;
    }
    TaskKey taskKey{"distidx/" + detail::sourceHex(lap.src) + "/" + lap.layer + "/" + lap.range.str() + "/" +
                    corrDist->version.hex() + "/" + keyHash(key)};
    entry = Impl::Lap{key, nullptr, true, false, taskKey, priority};
    if (s.state == DistanceState::Unknown) {
        s.state = DistanceState::Pending;
    }
    Future<IndexPtr> future = maths_.pool().submit(
        priority, std::move(taskKey), CancelToken{},
        [series = corrDist->series, key, priority](const CancelToken& cancel) -> Result<IndexPtr> {
            Result<analysis::DistanceIndex> built =
                analysis::DistanceIndex::build(series, key, priority, cancel);
            if (!built) {
                return Failure{built.error()};
            }
            return std::make_shared<const analysis::DistanceIndex>(std::move(built).value());
        });
    Dispatcher* dispatcher = &dispatcher_;       // runtime-owned (01 section 5.3 rule 11): the context
    const QPointer<DistanceService> guard(this); // made on the GUI thread, checked there
    future.onReady([dispatcher, guard, lap, key](const Result<IndexPtr>& result) {
        dispatcher->post(dispatcher, [guard, lap, key, result] {
            DistanceService* self = guard.data();
            if (self == nullptr) {
                return;
            }
            Impl::Source& source = self->impl_->sources[lap.src];
            const auto it = source.laps.find(lap);
            if (it == source.laps.end() || !(it->second.key == key) || !it->second.pending) {
                return; // forgotten or superseded
            }
            if (result && *result != nullptr && !((*result)->key() == key)) {
                return; // not built for this key (never expected: the task key carries the whole key)
            }
            it->second.pending = false;
            ++source.generation;
            if (result) {
                it->second.index = *result;
                source.state = DistanceState::Ready;
                Q_EMIT self->distanceReady(lap.src);
                return;
            }
            if (result.error().code == ErrorCode::Cancelled) {
                source.laps.erase(it); // asked again later
                return;
            }
            it->second.failed = true; // no valid Corr Dist in this lap: it stays without an index
            const bool anyIndex = std::any_of(source.laps.begin(), source.laps.end(),
                                              [](const auto& e) { return e.second.index != nullptr; });
            const bool anyPending = std::any_of(source.laps.begin(), source.laps.end(),
                                                [](const auto& e) { return e.second.pending; });
            if (!anyIndex && !anyPending && source.waiting.empty()) {
                self->setState(lap.src, DistanceState::NoDistance);
            } else {
                Q_EMIT self->distanceReady(lap.src); // chains rebuild without this lap
            }
        });
    });
    return nullptr;
}

std::shared_ptr<const plot::DistanceChain> DistanceService::chain(const model::RangeRef& rangeRef,
                                                                  Priority priority) {
    const auto src = maths_.source(rangeRef.src);
    const model::Range* range = src == nullptr ? nullptr : detail::findRange(*src, rangeRef);
    const model::DataLayer* layer = src == nullptr ? nullptr : detail::layerOf(*src, rangeRef.layer);
    if (range == nullptr || layer == nullptr) {
        return nullptr;
    }
    const model::RangeRef ref = normalised(rangeRef);
    Impl::Source& s = impl_->sources[ref.src];
    if (const auto cached = s.chains.find(ref);
        cached != s.chains.end() && cached->second.generation == s.generation &&
        cached->second.sourceVersion == src->version && cached->second.chain != nullptr) {
        return cached->second.chain;
    }
    // The outing around the range, and its laps in time order (laps partition [SOL, EOL), D-014).
    const model::Range* outing =
        range->kind == model::RangeKind::Outing ? range : detail::outingRangeAt(*layer, range->start);
    const Micros oStart = outing != nullptr ? outing->start : 0;
    const Micros oEnd = outing != nullptr ? outing->end : std::max(layer->duration, range->end);
    std::vector<std::pair<std::string_view, const model::Range*>> pieces;
    for (const model::Range* lap : detail::rangesWithin(*layer, detail::kLapsGroup, oStart, oEnd)) {
        pieces.emplace_back(detail::kLapsGroup, lap);
    }
    if (pieces.empty()) { // an outing without laps: one piece over the outing itself
        if (outing != nullptr) {
            pieces.emplace_back(detail::kOutingsGroup, outing);
        } else {
            pieces.emplace_back(ref.groupPath, range);
        }
    }
    std::vector<plot::LapPiece> lapPieces;
    bool anyIndexed = false;
    for (const auto& [group, lap] : pieces) {
        plot::LapPiece p;
        p.start = lap->start;
        p.end = lap->end;
        p.stretch = lap->stretch.value_or(1.0);
        p.idx = index(detail::makeRef(*src, ref.layer, group, *lap), priority);
        anyIndexed = anyIndexed || p.idx != nullptr;
        lapPieces.push_back(std::move(p));
    }
    if (!anyIndexed) {
        return nullptr;
    }
    auto made = std::make_shared<const plot::DistanceChain>(std::move(lapPieces), range->start);
    s.chains[ref] = Impl::Chain{s.generation, src->version, made};
    return made;
}

DistanceState DistanceService::state(const model::SourceId& source) const {
    const auto it = impl_->sources.find(source);
    return it == impl_->sources.end() ? DistanceState::Unknown : it->second.state;
}

void DistanceService::prefetchLaps(const model::SourceId& source) {
    const auto src = maths_.source(source);
    if (src == nullptr) {
        return;
    }
    for (const model::DataLayer& layer : src->layers) {
        const model::RangeGroup* laps = layer.ranges.find(detail::kLapsGroup);
        const model::RangeGroup* group = laps != nullptr ? laps : layer.ranges.find(detail::kOutingsGroup);
        if (group == nullptr) {
            continue;
        }
        for (const model::Range& r : group->ranges) {
            index(detail::makeRef(*src, layer.name, group->path, r), Priority::Background);
        }
    }
}

void DistanceService::prefetch(const model::SourceId& source) {
    impl_->sources[source].prefetch = true;
    prefetchLaps(source);
}

void DistanceService::forgetSource(const model::SourceId& source) {
    maths_.pool().cancelAll("distidx/" + detail::sourceHex(source) + "/");
    impl_->sources.erase(source);
}

void DistanceService::onCorrDistReady(const model::SourceId& source) {
    Impl::Source& s = impl_->sources[source];
    const auto waiting = std::move(s.waiting);
    s.waiting.clear();
    for (const auto& [lap, priority] : waiting) {
        index(lap, priority);
    }
    if (s.prefetch) {
        prefetchLaps(source);
    }
}

void DistanceService::onStatusChanged(const model::SourceId& source) {
    const auto src = maths_.source(source);
    if (src == nullptr) {
        return;
    }
    // Any layer's Corr Dist in Error/Disabled with no index at all: no distance data for the source.
    bool usable = false;
    for (const model::DataLayer& layer : src->layers) {
        const maths::ChannelKey key = maths::ChannelKey::make(source, layer.name, detail::kCorrDist);
        const maths::MathState st = maths_.status(key).state;
        usable = usable || (st != maths::MathState::Error && st != maths::MathState::Disabled);
    }
    if (!usable) {
        setState(source, DistanceState::NoDistance);
    } else if (state(source) == DistanceState::NoDistance) {
        impl_->sources[source].state = DistanceState::Pending; // re-evaluated after a settings change
        if (impl_->sources[source].prefetch) {
            prefetchLaps(source);
        }
    }
}

} // namespace esses::qt
