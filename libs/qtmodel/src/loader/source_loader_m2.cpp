// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's M2 members (07 section 5.1, 05 sections 2 and 8.1, 06 section 13.8; D-028, D-029, D-168):
// the step-4 inputs (the project's mappings and unit maps), the user's text profiles, the Mapping-panel
// hand-off (task A ends with a PendingResolution, the panel's decision submits task B by value, Cancel drops
// the pending open; no worker ever waits for the user, 01 section 5.1 rule W (d)) and the lap-stretch factors
// published by LapStretchService (D-016).

#include "../range_lookup.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QPointer>
#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace esses::qt {

namespace {

QString qPath(const std::string& utf8) {
    return QString::fromStdString(utf8);
}

formats::ImportLogEntry infoEntry(std::string code, std::string message) {
    formats::ImportLogEntry e;
    e.level = formats::ImportLogEntry::Level::Info;
    e.code = std::move(code);
    e.message = std::move(message);
    return e;
}

std::vector<std::filesystem::path> jsonFilesIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

void SourceLoader::setChannelDocuments(
    model::MappingTable mappings, model::AliasTable aliases,
    std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps) {
    mappings_ = std::move(mappings);
    aliases_ = std::move(aliases);
    unitMaps_ = std::move(unitMaps);
}

std::vector<std::string> SourceLoader::registerTextProfiles(const std::filesystem::path& dataDir,
                                                            const std::filesystem::path& configDir) {
    std::vector<std::string> log;
    if (pending_ > 0) {
        log.emplace_back("text profiles not registered: an open is in flight");
        return log;
    }
    const auto dictionaryFor =
        [&](const std::string& importerId) -> std::shared_ptr<const formats::Dictionary> {
        const std::string stem = formats::UnitMappingProfile::fileStem(importerId) + ".json";
        for (const std::filesystem::path& dir : {dataDir / "dictionaries", configDir / "dictionaries"}) {
            const std::filesystem::path file = dir / pathFromUtf8(stem);
            std::error_code ec;
            if (dir.empty() || !std::filesystem::is_regular_file(file, ec)) {
                continue;
            }
            Result<formats::Dictionary> d = formats::Dictionary::load(file, log);
            if (d) {
                return std::make_shared<const formats::Dictionary>(std::move(d).value());
            }
            log.push_back(pathToUtf8(file) + ": " + d.error().toString());
        }
        return nullptr;
    };
    const std::pair<std::filesystem::path, bool> dirs[] = {
        {dataDir.empty() ? std::filesystem::path() : dataDir / "profiles-text", true},
        {configDir.empty() ? std::filesystem::path() : configDir / "profiles-text", false}};
    for (const auto& [dir, shipped] : dirs) {
        for (const std::filesystem::path& file : jsonFilesIn(dir)) {
            Result<formats::TextProfile> profile = formats::TextProfile::load(file, log);
            if (!profile) {
                log.push_back(pathToUtf8(file) + ": " + profile.error().toString());
                continue;
            }
            const std::string id = profile->effectiveImporterId();
            if (registry_->find(id) != nullptr) {
                if (!shipped) {
                    log.push_back(pathToUtf8(file) + ": an importer '" + id + "' is already registered");
                }
                continue; // the shipped profiles are built in (D-168)
            }
            std::shared_ptr<const formats::Dictionary> dictionary = dictionaryFor(id);
            if (Result<void> r = registry_->addProfile(std::move(profile).value(), std::move(dictionary));
                !r) {
                log.push_back(pathToUtf8(file) + ": " + r.error().toString());
            }
        }
    }
    return log;
}

// ---- the Mapping-panel hand-off (05 sections 2 and 8.1)
// ------------------------------------------------------

void SourceLoader::openWithHandOff(const std::filesystem::path& path, formats::OpenContext context,
                                   int openId, CancelToken cancel) {
    std::shared_ptr<const formats::ImporterRegistry> registry = registry_;
    // Task A: packages the unresolved rows and the skeleton; never waits (the worker returns at once).
    formats::MappingHook hook = [](std::vector<formats::UnresolvedRow> rows,
                                   std::shared_ptr<const formats::OpenState> skeleton) {
        formats::PendingResolution pending;
        pending.rows = std::move(rows);
        pending.skeleton = std::move(skeleton);
        return pending;
    };
    Future<formats::LoadResult> future =
        registry->load(path, {}, pool_, context, std::move(cancel), std::move(hook));
    Dispatcher* dispatcher = &dispatcher_;
    const QPointer<SourceLoader> guard(this);
    // The registry borrowed by task A (and by the skeleton's reader) and the unit table stay alive until the
    // result reaches the GUI thread, also when this loader is gone by then.
    // A result whose open was cancelled meanwhile (cancelOpens: the project changed) is dropped.
    future.onReady([dispatcher, guard, path, openId, keep = registry,
                    units = units_](const Result<formats::LoadResult>& result) {
        static_cast<void>(units);
        if (!result) {
            const QString message = QString::fromStdString(result.error().toString());
            dispatcher->post(dispatcher, [guard, path, message, openId, keep] {
                SourceLoader* self = guard.data();
                if (self == nullptr || self->opens_.erase(openId) == 0) {
                    return;
                }
                --self->pending_;
                Q_EMIT self->openFailed(QString::fromStdString(pathToUtf8(path)), message);
            });
            return;
        }
        if (const auto* opened = std::get_if<formats::OpenedSource>(&*result)) {
            auto source = opened->source;
            auto log = opened->log;
            dispatcher->post(dispatcher, [guard, path, source, log, openId, keep]() mutable {
                SourceLoader* self = guard.data();
                if (self == nullptr || self->opens_.erase(openId) == 0) {
                    return;
                }
                --self->pending_;
                self->published(path, std::move(source), std::move(log));
            });
            return;
        }
        formats::PendingResolution pending = std::get<formats::PendingResolution>(*result);
        dispatcher->post(dispatcher, [guard, pending = std::move(pending), openId, keep]() mutable {
            SourceLoader* self = guard.data();
            if (self == nullptr || self->opens_.erase(openId) == 0) {
                return;
            }
            --self->pending_;
            self->mappingPending(std::move(pending));
        });
    });
}

void SourceLoader::mappingPending(formats::PendingResolution pending) {
    const int id = nextPendingId_++;
    const auto [it, inserted] = pendingMappings_.emplace(id, std::move(pending));
    static_cast<void>(inserted);
    Q_EMIT mappingRequired(id, it->second);
}

const formats::PendingResolution* SourceLoader::pendingMapping(int pendingId) const {
    const auto it = pendingMappings_.find(pendingId);
    return it == pendingMappings_.end() ? nullptr : &it->second;
}

void SourceLoader::resolveMapping(int pendingId, formats::MappingDecision decision) {
    const auto it = pendingMappings_.find(pendingId);
    if (it == pendingMappings_.end()) {
        return;
    }
    formats::PendingResolution pending = std::move(it->second);
    pendingMappings_.erase(it);
    const std::string path = pending.path;
    const std::size_t rows = pending.rows.size();
    const bool anyway = decision.openAnyway;
    ++pending_;
    CancelSource cancel;
    const CancelToken token = cancel.token();
    const int openId = track(pathFromUtf8(path), std::move(cancel));
    // Task B with the decision by value (05 section 2); the result is delivered like any open.
    Future<formats::OpenedSource> future =
        registry_->resume(std::move(pending), std::move(decision), pool_, openContext(), token);
    Q_EMIT importLog(
        qPath(path),
        {infoEntry("open.mapping_resolved",
                   anyway ? std::to_string(rows) + " unresolved channel(s) opened anyway: they stay unitless"
                          : std::to_string(rows) + " unresolved channel(s) resolved in the "
                                                   "Mapping panel")});
    deliver(pathFromUtf8(path), std::move(future), openId);
}

void SourceLoader::cancelMapping(int pendingId) {
    const auto it = pendingMappings_.find(pendingId);
    if (it == pendingMappings_.end()) {
        return;
    }
    const std::string path = it->second.path;
    pendingMappings_.erase(it);
    Q_EMIT importLog(qPath(path),
                     {infoEntry("open.mapping_cancelled", "the open was cancelled in the Mapping panel")});
}

// ---- lap stretching (D-016)
// ----------------------------------------------------------------------------------

bool SourceLoader::setRangeStretch(const model::SourceId& id, std::string_view layer,
                                   const std::map<model::RangeId, double>& stretch) {
    const std::shared_ptr<const model::DataSource> current = source(id);
    const model::RangeGroup* group =
        current == nullptr ? nullptr : detail::groupOf(*current, layer, detail::kLapsGroup);
    if (group == nullptr) {
        return false;
    }
    const auto wanted = [&stretch](const model::Range& r) -> std::optional<double> {
        const auto it = stretch.find(r.id);
        if (it == stretch.end() || !std::isfinite(it->second) || it->second <= 0.0 || it->second == 1.0) {
            return std::nullopt;
        }
        return it->second;
    };
    const bool changed = std::any_of(group->ranges.begin(), group->ranges.end(),
                                     [&](const model::Range& r) { return r.stretch != wanted(r); });
    if (!changed) {
        return false;
    }
    auto next = std::make_shared<model::DataSource>(*current);
    model::DataLayer* nl = next->layer(model::canonicalLayerName(layer));
    model::RangeGroup* g = nl == nullptr ? nullptr : nl->ranges.find(detail::kLapsGroup);
    if (g == nullptr) {
        return false;
    }
    // Range::stretch is derived display data (D-016): the group versions stay, so nothing that reads the
    // ranges is invalidated (D-140).
    for (model::Range& r : g->ranges) {
        r.stretch = wanted(r);
    }
    ++next->version;
    publishReplacement(std::move(next));
    // The laps' indexes now carry the new stretch (DistanceService copies them on request), which moves the
    // Track Sections of a lap measured backwards or outside the map's tolerance: the pass runs again with
    // them. No distanceReady announces such a copy, and its run key differs by the indexes' keys (D-204).
    startPhase2(id, std::string(model::canonicalLayerName(layer)));
    return true;
}

} // namespace esses::qt
