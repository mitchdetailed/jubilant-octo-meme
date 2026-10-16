// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader (ARCHITECTURE_OUTLINE section 2.1, 01 section 4.1, 05 section 2; AC-M1-01/AC-M1-10): the open
// pipeline on the TaskPool, publication on the GUI thread, built-in maths registration, the summary pass and
// the phase-2 range annotations.

#include "range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/registry.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/store/series.hpp>

#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace esses::qt {

namespace {

struct Published {
    std::shared_ptr<const model::DataSource> source;
    std::vector<formats::ImportLogEntry> log;
};

TaskKey openKey(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        canonical = path;
    }
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    std::int64_t mtime = 0;
    if (!ec) {
        const auto t = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            mtime = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
        }
    }
    return TaskKey{"open/" + pathToUtf8(canonical) + "/" + std::to_string(ec ? 0 : size) + "/" +
                   std::to_string(mtime) + "/a"};
}

QString toQString(const std::filesystem::path& p) {
    return QString::fromStdString(pathToUtf8(p));
}

formats::ImportLogEntry logEntry(formats::ImportLogEntry::Level level, std::string code,
                                 std::string message) {
    formats::ImportLogEntry e;
    e.level = level;
    e.code = std::move(code);
    e.message = std::move(message);
    return e;
}

// Coalesced sidecar writes (02 section 4 sidecar rule 4): 500 ms after the first edit that is not on disk.
constexpr int kSidecarCoalesceMs = 500;

} // namespace

// The generation of the sidecar document on disk, per source. A write takes the mutex and writes only a newer
// generation, so a Background write that finishes after a newer one (or after the flush of a closing source)
// never puts an older document back.
struct SourceLoader::SidecarSlot {
    std::mutex m;
    std::uint64_t written = 0;
};

namespace {

Result<void> writeSidecarFile(std::mutex& m, std::uint64_t& written, const model::SidecarDocument& doc,
                              const std::filesystem::path& path, std::uint64_t generation) {
    const std::lock_guard lock(m);
    if (generation <= written) {
        return ok(); // a newer document is on disk already
    }
    if (Result<void> r = doc.save(path); !r) {
        return r;
    }
    written = generation;
    return ok();
}

} // namespace

SourceLoader::SourceLoader(TaskPool& pool, Dispatcher& dispatcher,
                           std::shared_ptr<const units::UnitRegistry> units, QObject* parent)
    : QObject(parent), pool_(pool), dispatcher_(dispatcher), units_(std::move(units)),
      registry_(formats::ImporterRegistry::withBuiltins(units_.get())),
      rules_(analysis::RuleRunner::circuitDefault()), // until setRuleRunner: the built-in circuit rules
      sidecarTimer_(new QTimer(this)) {
    sidecarTimer_->setSingleShot(true);
    sidecarTimer_->setInterval(kSidecarCoalesceMs);
    connect(sidecarTimer_, &QTimer::timeout, this, [this] { writeDueSidecars(); });
}

SourceLoader::~SourceLoader() {
    // The edits not on disk yet are written now (exit: a queued write may be cancelled by the pool's
    // shutdown), and the opens in flight are cancelled.
    while (!sidecarsDue_.empty() || !sidecarsInFlight_.empty()) {
        flushSidecar(!sidecarsDue_.empty() ? sidecarsDue_.begin()->first : sidecarsInFlight_.begin()->first,
                     false);
    }
    for (auto& [id, inFlight] : opens_) {
        inFlight.cancel.cancel();
    }
    for (const auto& src : open_) {
        const std::string hex = detail::sourceHex(src->id);
        pool_.cancelAll("summary/" + hex);
        pool_.cancelAll("ranges/" + hex + "/");
    }
}

void SourceLoader::setRuleRunner(analysis::RuleRunner runner) {
    rules_ = std::move(runner);
}

void SourceLoader::setDistanceService(DistanceService* distance) {
    if (distance_ != nullptr) {
        disconnect(distance_, nullptr, this, nullptr);
    }
    distance_ = distance;
    if (distance_ != nullptr) {
        // More lap indexes: the Track Sections of their laps can be built (M3, D-199).
        connect(distance_, &DistanceService::distanceReady, this, [this](const model::SourceId& id) {
            if (trackMaps_ == nullptr) {
                return;
            }
            if (const auto src = source(id)) {
                for (const model::DataLayer& l : src->layers) {
                    startPhase2(id, l.name);
                }
            }
        });
    }
}

void SourceLoader::setTrackMaps(TrackMapService* maps) {
    if (trackMaps_ != nullptr) {
        disconnect(trackMaps_, nullptr, this, nullptr);
    }
    trackMaps_ = maps;
    ++trackGeneration_;
    if (trackMaps_ != nullptr) {
        connect(trackMaps_, &TrackMapService::mapChanged, this, [this](const QString& venue) {
            ++trackGeneration_;
            restartPhase2(venue.toStdString());
        });
        connect(trackMaps_, &QObject::destroyed, this, [this] { trackMaps_ = nullptr; });
    }
    restartPhase2(std::nullopt);
}

void SourceLoader::restartPhase2(const std::optional<std::string>& venue) {
    for (const std::shared_ptr<const model::DataSource>& src : open_) {
        if (venue && TrackMapService::venueOf(*src) != *venue) {
            continue;
        }
        for (const model::DataLayer& l : src->layers) {
            startPhase2(src->id, l.name);
        }
    }
}

void SourceLoader::setMathsService(MathsService* maths) {
    if (maths_ != nullptr) {
        disconnect(maths_, nullptr, this, nullptr);
    }
    maths_ = maths;
    if (maths_ != nullptr) {
        connect(maths_, &MathsService::channelReady, this, [this](const maths::ChannelKey& key) {
            if (key.nameCf == caseFold(detail::kCorrSpeed)) {
                startPhase2(key.src, key.layer);
            }
        });
    }
}

int SourceLoader::pendingOpens() const noexcept {
    return pending_;
}

const formats::ImporterRegistry& SourceLoader::registry() const {
    return *registry_;
}

std::shared_ptr<const model::DataSource> SourceLoader::source(const model::SourceId& id) const {
    for (const auto& s : open_) {
        if (s->id == id) {
            return s;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<const model::DataSource>> SourceLoader::sources() const {
    return open_;
}

formats::OpenContext SourceLoader::openContext() const {
    formats::OpenContext context;
    context.units = units_.get();
    context.pool = &pool_;
    context.loadSidecar = useSidecars_;
    context.rangeRebuild = rules_.rebuildFunction(); // shares the rules: may outlive this loader
    context.mappings = mappings_;                    // M2 step 4c
    context.unitMaps = unitMaps_;                    // M2 step 4b
    return context;
}

int SourceLoader::track(const std::filesystem::path& path, CancelSource cancel) {
    const int id = nextOpenId_++;
    opens_.emplace(id, InFlight{std::move(cancel), path});
    return id;
}

void SourceLoader::open(const std::filesystem::path& path) {
    ++pending_;
    formats::OpenContext context = openContext();
    CancelSource canceller;
    const CancelToken token = canceller.token();
    const int openId = track(path, std::move(canceller));
    if (mappingHandOff_) {
        openWithHandOff(path, std::move(context), openId, token); // M2: task A may end at the Mapping panel
        return;
    }
    // The task holds the registry and the units by shared_ptr (01 section 5.3 rule 9).
    Future<formats::OpenedSource> future =
        pool_.submit(Priority::Interactive, openKey(path), token,
                     [registry = std::shared_ptr<const formats::ImporterRegistry>(registry_), units = units_,
                      path, context](const CancelToken& cancel) {
                         static_cast<void>(units); // keeps context.units alive
                         return registry->open(path, {}, context, cancel);
                     });
    deliver(path, std::move(future), openId);
}

void SourceLoader::deliver(const std::filesystem::path& path, Future<formats::OpenedSource> future,
                           int openId) {
    Dispatcher* dispatcher = &dispatcher_;    // runtime-owned (01 section 5.3 rule 11): the context
    const QPointer<SourceLoader> guard(this); // made on the GUI thread, checked there
    // The registry and the unit table stay alive until the result is delivered (a task of the registry, e.g.
    // ImporterRegistry::resume, borrows them while it runs).
    future.onReady([dispatcher, guard, path, openId,
                    keep = std::shared_ptr<const formats::ImporterRegistry>(registry_),
                    units = units_](const Result<formats::OpenedSource>& result) {
        if (!result) {
            const QString message = QString::fromStdString(result.error().toString());
            dispatcher->post(dispatcher, [guard, path, message, openId, keep] {
                SourceLoader* self = guard.data();
                if (self == nullptr || self->opens_.erase(openId) == 0) {
                    return; // cancelled (cancelOpens): dropped, already counted
                }
                --self->pending_;
                Q_EMIT self->openFailed(toQString(path), message);
            });
            return;
        }
        static_cast<void>(units);
        Published p{result->source, result->log};
        dispatcher->post(dispatcher, [guard, path, p, openId, keep]() mutable {
            SourceLoader* self = guard.data();
            if (self == nullptr || self->opens_.erase(openId) == 0) {
                return; // cancelled (cancelOpens): never published
            }
            --self->pending_;
            self->published(path, std::move(p.source), std::move(p.log));
        });
    });
}

void SourceLoader::cancelOpens() {
    std::map<int, InFlight> opens = std::move(opens_);
    opens_.clear();
    pending_ -= static_cast<int>(opens.size());
    for (auto& [id, inFlight] : opens) {
        inFlight.cancel.cancel();
        Q_EMIT importLog(toQString(inFlight.path),
                         {logEntry(formats::ImportLogEntry::Level::Info, "open.cancelled",
                                   "the open was cancelled (the project changed or the files were closed)")});
    }
    std::map<int, formats::PendingResolution> waiting = std::move(pendingMappings_);
    pendingMappings_.clear();
    for (const auto& [id, pending] : waiting) {
        Q_EMIT importLog(
            QString::fromStdString(pending.path),
            {logEntry(formats::ImportLogEntry::Level::Info, "open.mapping_cancelled",
                      "the open waiting for the Mapping panel was cancelled (the project changed "
                      "or the files were closed)")});
    }
}

void SourceLoader::published(const std::filesystem::path& path,
                             std::shared_ptr<const model::DataSource> source,
                             std::vector<formats::ImportLogEntry> log) {
    if (source == nullptr) {
        Q_EMIT openFailed(toQString(path), tr("the importer returned no source"));
        return;
    }
    if (this->source(source->id) != nullptr) {
        // A second open of the same file (06 section 19 item 6): it is already in the Data window.
        formats::ImportLogEntry note;
        note.code = "open.already_open";
        note.message = "the file is already open";
        Q_EMIT importLog(toQString(path), {note});
        return;
    }
    open_.push_back(source);
    if (maths_ != nullptr) {
        maths_->addSource(source); // step 7: built-in maths registered and prefetched at Background
    }
    Q_EMIT sourcePublished(source);
    // The lap indexes only now: their prefetch requires Corr Dist, and MathsService::require applies an
    // evaluation that has already finished (an idle worker can finish the prefetch above while this thread is
    // still here) at once, announcing channelReady() for Corr Dist and Corr Speed on the spot. Before
    // sourcePublished(), that would hand its listeners a channel of a source the selection does not hold yet
    // (a trust edit made in reply was dropped).
    if (distance_ != nullptr && this->source(source->id) != nullptr) {
        distance_->prefetch(source->id);
    }
    Q_EMIT importLog(toQString(path), log);

    // One summary/<sourceId> task (02 section 3.2 rule 10): every logged channel's SummaryPyramid, streamed
    // without inserting chunks into the cache.
    std::vector<store::Column> columns;
    for (const model::DataLayer& layer : source->layers) {
        for (const model::Channel& c : layer.channels) {
            if (c.desc.source == model::ChannelSource::Logged) {
                columns.push_back(store::seriesValues(c.series));
            }
        }
    }
    if (!columns.empty()) {
        pool_.submit(Priority::Background, TaskKey{"summary/" + detail::sourceHex(source->id)}, CancelToken{},
                     [columns = std::move(columns)](const CancelToken& cancel) -> Result<void> {
                         for (const store::Column& c : columns) {
                             if (cancel.checkpoint()) {
                                 return fail(ErrorCode::Cancelled, "summary cancelled");
                             }
                             if (c.valid()) {
                                 static_cast<void>(c.buildSummary(cancel));
                             }
                         }
                         return ok();
                     });
    }
}

void SourceLoader::startPhase2(const model::SourceId& id, const std::string& layer) {
    const auto src = source(id);
    const model::DataLayer* l = src == nullptr ? nullptr : detail::layerOf(*src, layer);
    if (l == nullptr || maths_ == nullptr || l->ranges.find(detail::kLapsGroup) == nullptr) {
        return;
    }
    const maths::ChannelKey speedKey = maths::ChannelKey::make(id, layer, detail::kCorrSpeed);
    const std::optional<maths::ColumnHandle> speed = maths_->require(speedKey, Priority::Background);
    if (!speed) {
        return;
    }
    const std::string canonical(model::canonicalLayerName(layer));
    const std::string markers = model::markerSetHash(l->markers).hex();
    // M3: the Track Sections pass needs the venue's map and the laps' indexes (D-199); a lap without an index
    // yet gets no sections until distanceReady starts phase 2 again.
    // With the pass on, a source without a map runs it too: the run then removes stale Track Sections groups.
    const bool sectionsPass = trackMaps_ != nullptr && distance_ != nullptr && hasTrackSectionsRule();
    std::shared_ptr<const analysis::TrackMap> map;
    auto indexes = std::make_shared<std::map<model::RangeId, analysis::LapIndex>>();
    if (sectionsPass) {
        map = trackMaps_->mapFor(*src);
    }
    if (map != nullptr) {
        bool anyBeacon = false;
        for (const model::Marker& m : l->markers) {
            anyBeacon = anyBeacon || (m.cls == model::MarkerClass::BCN && m.enabled);
        }
        for (const model::Range& lap : l->ranges.find(detail::kLapsGroup)->ranges) {
            const model::RangeRef ref = detail::makeRef(*src, canonical, detail::kLapsGroup, lap);
            if (auto index = distance_->index(ref, Priority::Background)) {
                // 04 section 13 item 5: a lap that does not start at a BCN is measured from its end beacon;
                // one that does not end at a BCN keeps no section past its end (D-195 (d)).
                bool atBeacon = !anyBeacon;
                bool endsAtBeacon = !anyBeacon;
                for (const model::Marker& m : l->markers) {
                    const bool beacon = m.cls == model::MarkerClass::BCN && m.enabled;
                    atBeacon = atBeacon || (beacon && m.t == lap.start);
                    endsAtBeacon = endsAtBeacon || (beacon && m.t == lap.end);
                }
                indexes->emplace(lap.id, analysis::LapIndex{std::move(index), atBeacon, endsAtBeacon});
            }
        }
    }
    // One run per (layer, marker set, Corr Speed version[, map generation, indexed laps]): the task key and
    // the dedupe key are the same, so a newer Corr Speed, marker set, map or index never joins an older run.
    const std::string prefix = "ranges/" + detail::sourceHex(id) + "/" + canonical + "/";
    std::string run = prefix + markers + "/" + speed->version.hex();
    if (sectionsPass) {
        run += "/t" + std::to_string(trackGeneration_) + "/i" + std::to_string(indexes->size());
    }
    if (!phase2_.insert(run).second) {
        return; // this marker set was annotated (or is being annotated) with this Corr Speed already
    }
    pool_.cancelAll(prefix); // 02 section 4.4 rule 13: older ranges/<src>/<layer>/ runs are cancelled first
    Future<model::RangeGroupTree> future = pool_.submit(
        Priority::Interactive, TaskKey{run}, CancelToken{},
        [rules = rules_, src, canonical, series = speed->series, sectionsPass, map,
         indexes](const CancelToken& cancel) {
            if (!sectionsPass) {
                return rules.runPhase2(*src, canonical, std::optional<store::Series>(series), cancel);
            }
            const analysis::LapIndexLookup indexOf = [indexes](const model::Range& lap) {
                const auto it = indexes->find(lap.id);
                return it != indexes->end() ? it->second : analysis::LapIndex{};
            };
            return rules.runPhase2(*src, canonical, std::optional<store::Series>(series), map, indexOf,
                                   cancel);
        });
    Dispatcher* dispatcher = &dispatcher_;
    const QPointer<SourceLoader> guard(this);
    future.onReady([dispatcher, guard, src, canonical, markers,
                    run](const Result<model::RangeGroupTree>& result) {
        std::optional<model::RangeGroupTree> tree;
        if (result) {
            tree = *result;
        }
        dispatcher->post(dispatcher, [guard, src, canonical, markers, run, tree = std::move(tree)]() mutable {
            if (SourceLoader* self = guard.data()) {
                self->phase2Finished(src, canonical, markers, run, std::move(tree));
            }
        });
    });
}

void SourceLoader::phase2Finished(const std::shared_ptr<const model::DataSource>& annotated,
                                  const std::string& canonical, const std::string& markers,
                                  const std::string& run, std::optional<model::RangeGroupTree> tree) {
    const auto current = source(annotated->id);
    if (current == nullptr) {
        return; // closed
    }
    const model::DataLayer* l = detail::layerOf(*current, canonical);
    if (l == nullptr) {
        return;
    }
    if (!tree) {
        phase2_.erase(run); // cancelled or failed: a later request may run it again; the phase-1 tree stays
        return;
    }
    if (model::markerSetHash(l->markers).hex() != markers) {
        // The laps were rebuilt from another marker set since the run started: its annotations do not apply.
        // Annotate the current snapshot instead (its own run dedupes against one already started).
        phase2_.erase(run);
        startPhase2(current->id, canonical);
        return;
    }
    // Same marker set (the snapshot may be newer, e.g. a trust override committed meanwhile): only the
    // phase-2 annotations are taken over, by (range id, start, end), into the current tree, so the newer
    // snapshot's own edits survive.
    model::RangeGroupTree merged = l->ranges;
    bool changed = false;
    // M3: the Track Sections subtree of the run replaces the current one (D-199).
    for (const std::shared_ptr<const analysis::IRangeRule>& rule : rules_.rules()) {
        const auto* sections = dynamic_cast<const analysis::TrackSectionsRule*>(rule.get());
        if (sections == nullptr || !sections->appliesTo(l->name)) {
            continue;
        }
        const std::string prefix(model::parentGroupPath(sections->settings().produces));
        const model::RangeGroup* now = merged.find(prefix);
        const model::RangeGroup* built = tree->find(prefix);
        if ((now == nullptr) != (built == nullptr) ||
            (now != nullptr && built != nullptr && !(*now == *built))) {
            merged.removeGroup(prefix);
            if (built != nullptr) {
                merged.setGroup(*built);
            }
            changed = true;
        }
    }
    std::as_const(*tree).forEach([&](const model::RangeGroup& from) {
        model::RangeGroup* into = merged.find(from.path);
        if (into == nullptr) {
            return;
        }
        for (model::Range& r : into->ranges) {
            const auto it = std::find_if(from.ranges.begin(), from.ranges.end(), [&r](const model::Range& a) {
                return a.id == r.id && a.start == r.start && a.end == r.end;
            });
            if (it != from.ranges.end() && (r.stint != it->stint || r.pitStop != it->pitStop)) {
                r.stint = it->stint;
                r.pitStop = it->pitStop;
                changed = true;
            }
        }
    });
    if (!changed) {
        return;
    }
    merged.inheritVersions(l->ranges); // D-140: an annotated group's version moves on
    auto next = std::make_shared<model::DataSource>(*current);
    if (model::DataLayer* nl = next->layer(canonical)) {
        nl->ranges = std::move(merged);
    }
    ++next->version;
    publishReplacement(std::move(next));
}

bool SourceLoader::hasTrackSectionsRule() const {
    return std::any_of(rules_.rules().begin(), rules_.rules().end(), [](const auto& rule) {
        return dynamic_cast<const analysis::TrackSectionsRule*>(rule.get()) != nullptr;
    });
}

void SourceLoader::publishReplacement(std::shared_ptr<const model::DataSource> next) {
    const auto it =
        std::find_if(open_.begin(), open_.end(), [&](const auto& s) { return s->id == next->id; });
    if (it == open_.end()) {
        return;
    }
    *it = next;
    if (maths_ != nullptr) {
        maths_->replaceSource(next);
    }
    Q_EMIT sourceReplaced(next);
}

Result<std::shared_ptr<const model::DataSource>>
SourceLoader::commitSidecar(std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc) {
    if (current == nullptr || source(current->id) == nullptr) {
        return fail(ErrorCode::NotFound, "the source is not open");
    }
    current = source(current->id); // edits apply to the newest snapshot
    if (doc.sourceHash == model::SourceId{}) {
        // A first edit: the source block names this log (D-015, D-130), so the next open applies the
        // document.
        const model::SidecarDocument fresh = model::newSidecarFor(*current);
        doc.sourcePathRel = fresh.sourcePathRel;
        doc.sourceSize = fresh.sourceSize;
        doc.sourceHash = fresh.sourceHash;
        doc.sourceContentHash = fresh.sourceContentHash;
        doc.importerId = fresh.importerId;
        doc.importerVersion = fresh.importerVersion;
        doc.companions = fresh.companions;
    }
    doc.edited = model::DateTime::nowUtc();
    model::SidecarApplyReport report;
    std::shared_ptr<const model::DataSource> next =
        current->withSidecar(std::move(doc), &report, rules_.rebuildFunction());
    if (useSidecars_ && !next->prov.path.empty()) {
        // Written within 500 ms by a Background task; the edits in between coalesce (D-190).
        sidecarsDue_.insert_or_assign(next->id, SidecarDue{next->sidecar,
                                                           model::SidecarDocument::pathFor(next->prov.path),
                                                           ++sidecarGeneration_});
        if (!sidecarTimer_->isActive()) {
            sidecarTimer_->start();
        }
    }
    publishReplacement(next);
    if (!report.notes.empty()) {
        std::vector<formats::ImportLogEntry> notes;
        for (const model::ModelNote& n : report.notes) {
            formats::ImportLogEntry e;
            e.level = n.level == model::ModelNote::Level::Error     ? formats::ImportLogEntry::Level::Error
                      : n.level == model::ModelNote::Level::Warning ? formats::ImportLogEntry::Level::Warning
                                                                    : formats::ImportLogEntry::Level::Info;
            e.code = n.code;
            e.message = n.message;
            notes.push_back(std::move(e));
        }
        Q_EMIT importLog(toQString(next->prov.path), notes);
    }
    return next;
}

std::shared_ptr<SourceLoader::SidecarSlot> SourceLoader::slotFor(const model::SourceId& source) {
    std::shared_ptr<SidecarSlot>& slot = sidecarSlots_[source];
    if (slot == nullptr) {
        slot = std::make_shared<SidecarSlot>();
    }
    return slot;
}

void SourceLoader::writeDueSidecars() {
    std::map<model::SourceId, SidecarDue> due = std::move(sidecarsDue_);
    sidecarsDue_.clear();
    Dispatcher* dispatcher = &dispatcher_;
    const QPointer<SourceLoader> guard(this);
    for (auto& [id, d] : due) {
        // Kept until the write is known to be done, so a close or the destructor can write it on this thread
        // should the task still be queued (or be cancelled by the pool's shutdown).
        sidecarsInFlight_.insert_or_assign(id, d);
        // Unkeyed: the slot orders the writes of one source (the newest generation wins).
        Future<void> written =
            pool_.submit(Priority::Background, TaskKey{}, CancelToken{},
                         [slot = slotFor(id), doc = std::move(d.doc), path = d.path,
                          generation = d.generation](const CancelToken&) -> Result<void> {
                             return writeSidecarFile(slot->m, slot->written, doc, path, generation);
                         });
        written.onReady([dispatcher, guard, id, path = d.path,
                         generation = d.generation](const Result<void>& r) {
            const bool failed = !r && r.error().code != ErrorCode::Cancelled;
            const std::string message =
                failed ? "the sidecar could not be written: " + r.error().toString() : std::string();
            dispatcher->post(dispatcher, [guard, id, path, generation, failed, message] {
                SourceLoader* self = guard.data();
                if (self == nullptr) {
                    return;
                }
                const auto it = self->sidecarsInFlight_.find(id);
                if (it != self->sidecarsInFlight_.end() && it->second.generation == generation) {
                    self->sidecarsInFlight_.erase(it);
                }
                if (failed) {
                    Q_EMIT self->importLog(toQString(path), {logEntry(formats::ImportLogEntry::Level::Warning,
                                                                      "sidecar.write_failed", message)});
                }
            });
        });
    }
}

void SourceLoader::flushSidecar(const model::SourceId& source, bool report) {
    // The newest document of the source: the one due, else the one whose Background write may not have run.
    std::optional<SidecarDue> latest;
    if (const auto it = sidecarsDue_.find(source); it != sidecarsDue_.end()) {
        latest = std::move(it->second);
        sidecarsDue_.erase(it);
    }
    if (const auto it = sidecarsInFlight_.find(source); it != sidecarsInFlight_.end()) {
        if (!latest) {
            latest = std::move(it->second);
        }
        sidecarsInFlight_.erase(it);
    }
    if (!latest) {
        return;
    }
    const SidecarDue& due = *latest;
    const std::shared_ptr<SidecarSlot> slot = slotFor(source);
    // On this thread: a Background write of an older document in flight finishes first (the slot's mutex).
    if (Result<void> r = writeSidecarFile(slot->m, slot->written, due.doc, due.path, due.generation);
        !r && report) {
        Q_EMIT importLog(toQString(due.path),
                         {logEntry(formats::ImportLogEntry::Level::Warning, "sidecar.write_failed",
                                   "the sidecar could not be written: " + r.error().toString())});
    }
}

void SourceLoader::close(const model::SourceId& id) {
    const auto it = std::find_if(open_.begin(), open_.end(), [&](const auto& s) { return s->id == id; });
    if (it == open_.end()) {
        return;
    }
    flushSidecar(id, true); // the edits not on disk yet, before the source goes (D-190)
    open_.erase(it);
    const std::string hex = detail::sourceHex(id);
    pool_.cancelAll("chunk/" + hex);
    pool_.cancelAll("summary/" + hex);
    pool_.cancelAll("ranges/" + hex + "/");
    if (distance_ != nullptr) {
        distance_->forgetSource(id);
    }
    if (maths_ != nullptr) {
        maths_->removeSource(id);
    }
    registry_->release(id);
    Q_EMIT sourceClosed(id);
}

void SourceLoader::closeAll() {
    cancelOpens();
    while (!open_.empty()) {
        close(open_.back()->id);
    }
}

void connectSourceLoader(SourceLoader& loader, DataSelectionModel& selection, MessagesModel* messages) {
    QObject::connect(&loader, &SourceLoader::sourcePublished, &selection,
                     &DataSelectionModel::sourcePublished);
    QObject::connect(&loader, &SourceLoader::sourceReplaced, &selection, &DataSelectionModel::sourceReplaced);
    QObject::connect(&loader, &SourceLoader::sourceClosed, &selection, &DataSelectionModel::closeSource);
    // The selection may outlive the loader (MainWindow destroys the loader first): a commit after that is
    // dropped instead of reaching a destroyed loader. With a project session the edit is an undoable
    // SidecarCommand on its stack (D-032, 06 section 5.2 setTrusted); its redo() commits.
    selection.setSidecarCommit(
        [guard = QPointer<SourceLoader>(&loader), session = QPointer<ProjectSession>(selection.session()),
         sel = QPointer<DataSelectionModel>(&selection)](std::shared_ptr<const model::DataSource> current,
                                                         model::SidecarDocument doc) {
            SourceLoader* l = guard.data();
            if (l == nullptr || current == nullptr) {
                return;
            }
            if (ProjectSession* s = session.data()) {
                const std::shared_ptr<const model::DataSource> newest = l->source(current->id);
                if (newest != nullptr) {
                    const QString text = sel.isNull() || sel->sidecarCommitText().isEmpty()
                                             ? QObject::tr("Edit trust")
                                             : sel->sidecarCommitText();
                    s->undoStack().push(
                        new SidecarCommand(*l, newest->id, newest->sidecar, std::move(doc), text));
                }
                return;
            }
            static_cast<void>(l->commitSidecar(std::move(current), std::move(doc)));
        });
    if (messages != nullptr) {
        QObject::connect(&loader, &SourceLoader::importLog, messages, &MessagesModel::appendImportLog);
        QObject::connect(&loader, &SourceLoader::openFailed, messages,
                         [messages](const QString& path, const QString& text) {
                             Message m;
                             m.severity = Message::Severity::Error;
                             m.origin = Message::Origin::Import;
                             m.source = path;
                             m.code = QStringLiteral("open.failed");
                             m.text = text;
                             messages->append(std::move(m));
                         });
        QObject::connect(&selection, &DataSelectionModel::hint, messages, [messages](const QString& text) {
            Message m;
            m.severity = Message::Severity::Info;
            m.origin = Message::Origin::Project;
            m.text = text;
            messages->append(std::move(m));
        });
    }
}

} // namespace esses::qt
