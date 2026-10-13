// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretchService (D-016, D-172): the lap-stretch toggle and the per-lap factors published as
// Range::stretch, computed from the laps' unstretched DistanceIndexes and the source's track length.

#include "../range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/model/detail.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QTimer>
#include <cmath>
#include <utility>
#include <variant>

namespace esses::qt {

namespace {

constexpr std::string_view kVenueLengthCf = "venue length";

} // namespace

LapStretchService::LapStretchService(SourceLoader& loader, DistanceService& distance,
                                     std::shared_ptr<const units::UnitRegistry> units,
                                     ProjectSession* session, QObject* parent)
    : QObject(parent), loader_(loader), distance_(distance), units_(std::move(units)), session_(session),
      timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(0); // after the current signal has reached every receiver (no nested republish)
    connect(timer_, &QTimer::timeout, this, [this] {
        const std::set<model::SourceId> sources = std::move(dirty_);
        dirty_.clear();
        for (const model::SourceId& id : sources) {
            update(id);
        }
    });
    connect(&loader_, &SourceLoader::sourcePublished, this,
            [this](const std::shared_ptr<const model::DataSource>& src) { scheduleUpdate(src->id); });
    connect(&loader_, &SourceLoader::sourceReplaced, this,
            [this](const std::shared_ptr<const model::DataSource>& src) { scheduleUpdate(src->id); });
    connect(&loader_, &SourceLoader::sourceClosed, this, [this](const model::SourceId& id) {
        trackLengths_.erase(id);
        dirty_.erase(id);
    });
    connect(&distance_, &DistanceService::distanceReady, this,
            [this](const model::SourceId& id) { scheduleUpdate(id); });
    if (session_ != nullptr) {
        connect(session_, &ProjectSession::lapStretchChanged, this, [this] { readSession(); });
        connect(session_, &ProjectSession::documentsChanged, this, [this] { readSession(); });
        readSession();
    }
}

LapStretchService::~LapStretchService() = default;

void LapStretchService::readSession() {
    const analysis::LapStretchSettings s = session_->lapStretchSettings();
    if (s == settings_) {
        return;
    }
    const bool toggled = s.enabled != settings_.enabled;
    settings_ = s;
    if (toggled) {
        Q_EMIT enabledChanged(settings_.enabled);
    }
    scheduleAll();
}

void LapStretchService::setEnabled(bool on) {
    if (session_ != nullptr) {
        session_->setLapStretchEnabled(on); // lapStretchChanged -> readSession
        return;
    }
    if (on == settings_.enabled) {
        return;
    }
    settings_.enabled = on;
    Q_EMIT enabledChanged(on);
    scheduleAll();
}

void LapStretchService::setSettings(analysis::LapStretchSettings settings) {
    if (session_ != nullptr) {
        setEnabled(settings.enabled);
        return;
    }
    if (settings == settings_) {
        return;
    }
    const bool toggled = settings.enabled != settings_.enabled;
    settings_ = settings;
    if (toggled) {
        Q_EMIT enabledChanged(settings_.enabled);
    }
    scheduleAll();
}

void LapStretchService::setTrackLength(const model::SourceId& source, std::optional<double> metres) {
    if (metres && std::isfinite(*metres) && *metres > 0.0) {
        trackLengths_[source] = *metres;
    } else {
        trackLengths_.erase(source);
    }
    scheduleUpdate(source);
}

std::optional<double> LapStretchService::trackLength(const model::SourceId& source) const {
    if (const auto it = trackLengths_.find(source); it != trackLengths_.end()) {
        return it->second;
    }
    const std::shared_ptr<const model::DataSource> src = loader_.source(source);
    if (src == nullptr) {
        return std::nullopt;
    }
    for (const model::Detail& d : src->details) {
        if (caseFold(d.id) != kVenueLengthCf) {
            continue;
        }
        const auto* q = std::get_if<model::Quantityd>(&d.value);
        if (q == nullptr) {
            return std::nullopt;
        }
        double metres = q->v; // a unitless length is taken as metres
        if (q->unit != units::kNoUnit && units_ != nullptr) {
            metres = model::toBase(*q, *units_).value_or(q->v);
        }
        return std::isfinite(metres) && metres > 0.0 ? std::optional<double>(metres) : std::nullopt;
    }
    return std::nullopt;
}

std::vector<analysis::LapStretchRow> LapStretchService::report(const model::SourceId& source) const {
    const std::shared_ptr<const model::DataSource> src = loader_.source(source);
    const model::RangeGroup* laps =
        src == nullptr ? nullptr : detail::groupOf(*src, model::kNormalLayer, detail::kLapsGroup);
    if (laps == nullptr) {
        return {};
    }
    std::vector<std::shared_ptr<const analysis::DistanceIndex>> keep;
    std::vector<std::pair<model::RangeId, const analysis::DistanceIndex*>> rows;
    for (const model::Range& r : laps->ranges) {
        keep.push_back(distance_.index(detail::makeRef(*src, model::kNormalLayer, detail::kLapsGroup, r),
                                       Priority::Background));
        rows.emplace_back(r.id, keep.back().get());
    }
    return analysis::LapStretch::report(rows, trackLength(source).value_or(0.0), settings_);
}

void LapStretchService::update(const model::SourceId& source) {
    const std::shared_ptr<const model::DataSource> src = loader_.source(source);
    const model::RangeGroup* laps =
        src == nullptr ? nullptr : detail::groupOf(*src, model::kNormalLayer, detail::kLapsGroup);
    if (laps == nullptr) {
        return;
    }
    const double track = trackLength(source).value_or(0.0);
    std::map<model::RangeId, double> factors;
    for (const model::Range& r : laps->ranges) {
        if (!settings_.enabled || track <= 0.0) {
            factors[r.id] = 1.0; // off: every lap unstretched, without waiting for an index
            continue;
        }
        const std::shared_ptr<const analysis::DistanceIndex> index = distance_.index(
            detail::makeRef(*src, model::kNormalLayer, detail::kLapsGroup, r), Priority::Background);
        if (index == nullptr) {
            factors[r.id] = r.stretch.value_or(1.0); // pending (or no distance): unchanged until it arrives
            continue;
        }
        factors[r.id] = analysis::LapStretch::row(*index, r.id, track, settings_).stretch;
    }
    static_cast<void>(loader_.setRangeStretch(source, model::kNormalLayer, factors));
    Q_EMIT reportChanged(source);
}

void LapStretchService::scheduleUpdate(const model::SourceId& source) {
    dirty_.insert(source);
    if (!timer_->isActive()) {
        timer_->start();
    }
}

void LapStretchService::scheduleAll() {
    for (const auto& src : loader_.sources()) {
        dirty_.insert(src->id);
    }
    if (!dirty_.empty() && !timer_->isActive()) {
        timer_->start();
    }
}

} // namespace esses::qt
