// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession's M2 members (07 section 5.1, D-019, D-032, D-161): the document setters the undo commands
// write through, the view state written without the stack (Data-window filters, the lap-stretch toggle), the
// options the services read (maths compat), and the journal: autosave, recovery and the autosave timer (02
// section 6.8, AC-M2-06).

#include <esses/base/json_doc.hpp>
#include <esses/qtmodel/project_session.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

QJsonObject objectOf(const JsonDoc& doc) {
    if (!doc.isObject()) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromStdString(doc.compact())).object();
}

JsonDoc docOf(const QJsonObject& o) {
    Result<JsonDoc> d = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
    return d ? std::move(d).value() : JsonDoc{};
}

} // namespace

void ProjectSession::setDirty(bool dirty) {
    if (dirty_ == dirty) {
        return;
    }
    dirty_ = dirty;
    Q_EMIT dirtyChanged(dirty);
}

void ProjectSession::markDirty() {
    setDirty(true);
}

// ---- document setters (the undo commands' redo()/undo())
// ----------------------------------------------------

void ProjectSession::setProjectFile(project::ProjectFile doc) {
    // View state and the members other documents own stay as they are (an undone options edit never rewinds
    // the selection, the filters, the MRU, the workbook list or the module order).
    doc.lastSelection = project_.lastSelection;
    doc.recentSources = project_.recentSources;
    doc.dataWindow = project_.dataWindow;
    doc.workbooks = project_.workbooks;
    doc.mathsOrder = project_.mathsOrder;
    doc.dirty = project_.dirty;
    // The lap-stretch toggle is view state as well (options.lapStretch.enabled, the toolbar's): kept.
    const bool stretch = project_.lapStretchSettings().enabled;
    if (doc.lapStretchSettings().enabled != stretch) {
        QJsonObject options = objectOf(doc.options);
        QJsonObject s = options.value(QStringLiteral("lapStretch")).toObject();
        s.insert(QStringLiteral("enabled"), stretch);
        options.insert(QStringLiteral("lapStretch"), s);
        doc.options = docOf(options);
    }
    project_ = std::move(doc);
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setOverrides(project::ChannelOverrides doc) {
    overrides_ = std::move(doc);
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setAliases(project::Aliases doc) {
    aliases_ = std::move(doc);
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setMappings(project::Mappings doc) {
    mappings_ = std::move(doc);
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setUnitMap(const std::string& stem, formats::UnitMappingProfile doc) {
    if (doc.rules.empty() && doc.importer.empty() && doc.unknown.isNull()) {
        unitMaps_.erase(stem);
    } else {
        unitMaps_.insert_or_assign(stem, std::move(doc));
    }
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setMathsModules(std::vector<maths::MathModule> modules) {
    mathsModules_ = std::move(modules);
    for (maths::MathModule& m : mathsModules_) {
        m.origin = maths::MathModule::Origin::Global;
    }
    project_.mathsOrder.clear();
    for (const maths::MathModule& m : mathsModules_) {
        project_.mathsOrder.push_back(m.id);
    }
    markDirty();
    Q_EMIT documentsChanged();
}

void ProjectSession::setWorkbookSet(WorkbookSet set) {
    set.names.resize(set.workbooks.size());
    for (std::size_t i = 0; i < set.workbooks.size(); ++i) {
        if (set.names[i].empty()) {
            set.names[i] = set.workbooks[i].name;
        }
    }
    workbooks_ = std::move(set.workbooks);
    workbookNames_ = std::move(set.names);
    project_.workbooks = workbookNames_;
    const int count = static_cast<int>(workbooks_.size());
    activeWorkbook_ = count == 0 ? 0 : std::clamp(activeWorkbook_, 0, count - 1);
    markDirty();
    Q_EMIT documentsChanged();
}

// ---- options the services read, view state
// ------------------------------------------------------------------

maths::MathsCompat ProjectSession::mathsCompat() const {
    maths::MathsCompat compat;
    const QJsonObject c = objectOf(project_.options)
                              .value(QStringLiteral("maths"))
                              .toObject()
                              .value(QStringLiteral("compat"))
                              .toObject();
    if (const QJsonValue v = c.value(QStringLiteral("unaryMinusAbovePower")); v.isBool()) {
        compat.unaryMinusAbovePower = v.toBool();
    }
    return compat;
}

bool ProjectSession::looseChannelSearch() const {
    const QJsonValue v = objectOf(project_.options)
                             .value(QStringLiteral("general"))
                             .toObject()
                             .value(QStringLiteral("looseChannelSearch"));
    return v.isBool() && v.toBool();
}

void ProjectSession::setDataWindow(JsonDoc dataWindow) {
    project_.dataWindow = std::move(dataWindow);
}

void ProjectSession::setLapStretchEnabled(bool enabled) {
    if (project_.lapStretchSettings().enabled == enabled) {
        return;
    }
    QJsonObject options = objectOf(project_.options);
    QJsonObject stretch = options.value(QStringLiteral("lapStretch")).toObject();
    stretch.insert(QStringLiteral("enabled"), enabled);
    if (!stretch.contains(QStringLiteral("tolerancePct"))) {
        stretch.insert(QStringLiteral("tolerancePct"), 5.0);
    }
    options.insert(QStringLiteral("lapStretch"), stretch);
    project_.options = docOf(options);
    Q_EMIT lapStretchChanged(enabled);
}

// ---- journal (02 section 6.8)
// -------------------------------------------------------------------------------

std::chrono::milliseconds ProjectSession::autosaveInterval() const {
    if (autosaveOverride_.count() > 0) {
        return autosaveOverride_;
    }
    return std::chrono::minutes(project_.autosaveMinutes());
}

void ProjectSession::setAutosaveInterval(std::chrono::milliseconds interval) {
    autosaveOverride_ = interval.count() > 0 ? interval : std::chrono::milliseconds(0);
    restartAutosaveTimer();
}

void ProjectSession::restartAutosaveTimer() {
    const std::chrono::milliseconds interval = autosaveInterval();
    if (!folder_ || interval.count() <= 0) {
        autosaveTimer_->stop();
        return;
    }
    const int ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(interval.count(), 0x7FFFFFFF));
    if (autosaveTimer_->isActive() && autosaveTimer_->interval() == ms) {
        return; // edits never postpone the next autosave
    }
    autosaveTimer_->start(ms);
}

Result<void> ProjectSession::autosave() {
    if (!folder_ || journal_ == nullptr) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    // While a recovery is put off (Later) this session journals its own edits beside the other session's
    // files, which stay untouched (D-187): a crash loses at most one autosave interval either way.
    return journalDirty(false);
}

Result<void> ProjectSession::journalDirty(bool force) {
    // The documents that differ from disk, by hash first: an autosave whose set is unchanged since the last
    // file, while that file is still in the journal, builds no entries (no parse, no canonical copy, no
    // write; D-190).
    std::vector<std::pair<std::string, std::string>> dirty;
    std::map<std::string, Hash128, std::less<>> hashes;
    for (auto& [rel, text] : documentTexts()) {
        if (text.empty()) {
            continue;
        }
        const auto b = baseline_.find(rel);
        if (b != baseline_.end() && b->second.failed) {
            continue;
        }
        const Hash128 h = hash128(text);
        if (b != baseline_.end() && b->second.text == h) {
            continue; // as on disk (or an untouched empty document that has no file)
        }
        hashes.insert_or_assign(rel, h);
        dirty.emplace_back(rel, std::move(text));
    }
    if (dirty.empty()) {
        // Nothing differs from disk: this session's older files are superseded too.
        if (!lastJournaled_.empty()) {
            if (Result<void> r = journal_->clear(project::JournalSessions::Own); !r) {
                return r;
            }
            lastJournaled_.clear();
        }
        return ok();
    }
    // Unchanged since the last file, which is still there: nothing to write. Another instance's clear (a
    // Discard on its recovery prompt) may have deleted that file; then the same set is written again.
    if (!force && hashes == lastJournaled_ && journal_->ownFileExists()) {
        return ok();
    }
    std::vector<project::JournalEntry> entries;
    entries.reserve(dirty.size());
    for (const auto& [rel, text] : dirty) {
        const auto b = baseline_.find(rel);
        Result<project::JournalEntry> e = project::JournalEntry::of(
            rel, text, b != baseline_.end() ? b->second.disk : std::optional<Hash128>{});
        if (!e) {
            return Failure{e.error()};
        }
        entries.push_back(std::move(e).value());
    }
    if (Result<void> r = journal_->append(entries); !r) {
        return r;
    }
    lastJournaled_ = std::move(hashes);
    return ok();
}

bool ProjectSession::hasRecoverableJournal() const {
    return journal_ != nullptr && journal_->pending(project::JournalSessions::Others);
}

std::optional<project::JournalReplay> ProjectSession::journalReplay() const {
    if (replayCache_) {
        return replayCache_;
    }
    if (!hasRecoverableJournal()) {
        return std::nullopt;
    }
    Result<project::JournalReplay> replay = journal_->replay(project::JournalSessions::Others);
    if (!replay) {
        return std::nullopt;
    }
    return std::move(replay).value();
}

void ProjectSession::offerRecovery() {
    replayCache_.reset();
    if (!hasRecoverableJournal()) {
        return;
    }
    // One replay per open (D-190): the prompt shows it and Recover applies it.
    Result<project::JournalReplay> replay = journal_->replay(project::JournalSessions::Others);
    if (!replay) {
        recoveryOutstanding_ = true;
        Q_EMIT logMessage(tr("The journal could not be replayed: %1")
                              .arg(QString::fromStdString(replay.error().toString())));
        return;
    }
    if (replay->entries.empty() && replay->skipped == 0) {
        // Every document it holds is on disk already (a save stopped before deleting it): nothing to offer.
        if (Result<void> r = journal_->clear(project::JournalSessions::Others); !r) {
            Q_EMIT logMessage(
                tr("The journal could not be deleted: %1").arg(QString::fromStdString(r.error().toString())));
        } else {
            Q_EMIT logMessage(tr("The journal held only changes that were already saved; it was deleted"));
        }
        recoveryOutstanding_ = hasRecoverableJournal();
        return;
    }
    if (!recoveryPrompt_) {
        replayCache_ = std::move(replay).value();
        recoveryOutstanding_ = true;
        Q_EMIT recoveryAvailable();
        return;
    }
    switch (recoveryPrompt_(*replay)) {
    case project::JournalRecovery::Recover:
        if (Result<void> r = applyReplay(*replay); !r) {
            recoveryOutstanding_ = hasRecoverableJournal();
            Q_EMIT logMessage(
                tr("The journal could not be applied: %1").arg(QString::fromStdString(r.error().toString())));
        }
        return;
    case project::JournalRecovery::Discard:
        if (Result<void> r = discardJournal(); !r) {
            Q_EMIT logMessage(
                tr("The journal could not be deleted: %1").arg(QString::fromStdString(r.error().toString())));
        }
        recoveryOutstanding_ = hasRecoverableJournal();
        return;
    case project::JournalRecovery::Later:
        break;
    }
    recoveryOutstanding_ = hasRecoverableJournal(); // Later keeps it
}

Result<void> ProjectSession::recoverJournal() {
    if (replayCache_) {
        const project::JournalReplay replay = std::move(*replayCache_);
        replayCache_.reset();
        return applyReplay(replay);
    }
    if (!hasRecoverableJournal()) {
        return fail(ErrorCode::NotFound, "no journal to recover");
    }
    Result<project::JournalReplay> replay = journal_->replay(project::JournalSessions::Others);
    if (!replay) {
        return Failure{replay.error()};
    }
    return applyReplay(*replay);
}

Result<void> ProjectSession::discardJournal() {
    if (journal_ == nullptr) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    replayCache_.reset();
    if (Result<void> r = journal_->clear(project::JournalSessions::Others); !r) {
        return r;
    }
    recoveryOutstanding_ = false;
    return ok();
}

Result<void> ProjectSession::discardSessionJournal() {
    if (journal_ == nullptr) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    if (Result<void> r = journal_->clear(project::JournalSessions::Own); !r) {
        return r;
    }
    lastJournaled_.clear();
    return ok();
}

} // namespace esses::qt
