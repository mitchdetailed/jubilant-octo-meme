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
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/series.hpp>

#include <QPointer>
#include <algorithm>
#include <chrono>
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

} // namespace

SourceLoader::SourceLoader(TaskPool& pool, Dispatcher& dispatcher,
                           std::shared_ptr<const units::UnitRegistry> units, QObject* parent)
    : QObject(parent), pool_(pool), dispatcher_(dispatcher), units_(std::move(units)),
      registry_(formats::ImporterRegistry::withBuiltins(units_.get())),
      rules_(analysis::RuleRunner::circuitDefault()) {} // until setRuleRunner: the built-in circuit rules

SourceLoader::~SourceLoader() {
    for (const auto& src : open_) {
        const std::string hex = detail::sourceHex(src->id);
        pool_.cancelAll("summary/" + hex);
        pool_.cancelAll("ranges/" + hex + "/");
    }
}

void SourceLoader::setRuleRunner(analysis::RuleRunner runner) {
    rules_ = std::move(runner);
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

void SourceLoader::open(const std::filesystem::path& path) {
    ++pending_;
    formats::OpenContext context;
    context.units = units_.get();
    context.pool = &pool_;
    context.loadSidecar = useSidecars_;
    context.rangeRebuild = rules_.rebuildFunction(); // shares the rules: may outlive this loader
    // The task holds the registry and the units by shared_ptr (01 section 5.3 rule 9).
    Future<formats::OpenedSource> future =
        pool_.submit(Priority::Interactive, openKey(path), CancelToken{},
                     [registry = std::shared_ptr<const formats::ImporterRegistry>(registry_), units = units_,
                      path, context](const CancelToken& cancel) {
                         static_cast<void>(units); // keeps context.units alive
                         return registry->open(path, {}, context, cancel);
                     });
    Dispatcher* dispatcher = &dispatcher_;    // runtime-owned (01 section 5.3 rule 11): the context
    const QPointer<SourceLoader> guard(this); // made on the GUI thread, checked there
    future.onReady([dispatcher, guard, path](const Result<formats::OpenedSource>& result) {
        if (!result) {
            const QString message = QString::fromStdString(result.error().toString());
            dispatcher->post(dispatcher, [guard, path, message] {
                if (SourceLoader* self = guard.data()) {
                    --self->pending_;
                    Q_EMIT self->openFailed(toQString(path), message);
                }
            });
            return;
        }
        Published p{result->source, result->log};
        dispatcher->post(dispatcher, [guard, path, p]() mutable {
            if (SourceLoader* self = guard.data()) {
                --self->pending_;
                self->published(path, std::move(p.source), std::move(p.log));
            }
        });
    });
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
    if (distance_ != nullptr) {
        distance_->prefetch(source->id);
    }
    Q_EMIT sourcePublished(source);
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
    // One run per (layer, marker set, Corr Speed version): the task key and the dedupe key are the same, so a
    // newer Corr Speed or marker set never joins an older run.
    const std::string prefix = "ranges/" + detail::sourceHex(id) + "/" + canonical + "/";
    const std::string run = prefix + markers + "/" + speed->version.hex();
    if (!phase2_.insert(run).second) {
        return; // this marker set was annotated (or is being annotated) with this Corr Speed already
    }
    pool_.cancelAll(prefix); // 02 section 4.4 rule 13: older ranges/<src>/<layer>/ runs are cancelled first
    Future<model::RangeGroupTree> future = pool_.submit(
        Priority::Interactive, TaskKey{run}, CancelToken{},
        [rules = rules_, src, canonical, series = speed->series](const CancelToken& cancel) {
            return rules.runPhase2(*src, canonical, std::optional<store::Series>(series), cancel);
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
    Result<void> saved;
    if (useSidecars_ && !next->prov.path.empty()) {
        saved = next->sidecar.save(model::SidecarDocument::pathFor(next->prov.path));
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
    if (!saved) {
        return Failure{saved.error()};
    }
    return next;
}

void SourceLoader::close(const model::SourceId& id) {
    const auto it = std::find_if(open_.begin(), open_.end(), [&](const auto& s) { return s->id == id; });
    if (it == open_.end()) {
        return;
    }
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
    // dropped instead of reaching a destroyed loader.
    selection.setSidecarCommit(
        [guard = QPointer<SourceLoader>(&loader)](std::shared_ptr<const model::DataSource> current,
                                                  model::SidecarDocument doc) {
            if (SourceLoader* l = guard.data()) {
                static_cast<void>(l->commitSidecar(std::move(current), std::move(doc)));
            }
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
